// Package store is nickbot's registry: the nicknames, who holds them, the names being held for
// their old owner, the events already handled, and the history, in one SQLite file.
//
// The rules of spec 3.2 live here, in one transaction each, so that two commands arriving together
// cannot both win a name. The driver is pure Go (modernc.org/sqlite): the process also loads
// SQLCipher through libbmbot, and two copies of SQLite's symbols in one process interpose.
package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"strings"
	"time"

	_ "modernc.org/sqlite"

	"github.com/yshurik/ynotbit/nickbot/internal/nick"
)

const (
	// Quarantine is how long a name an address gave up is kept free for that address alone.
	Quarantine = 30 * 24 * time.Hour
	// HandledKeep is how long the id of a handled event is remembered.
	HandledKeep = 7 * 24 * time.Hour
	// HistoryKeep is how long history rows are kept.
	HistoryKeep = 365 * 24 * time.Hour
)

var (
	ErrTaken    = errors.New("the nickname is taken")
	ErrReserved = errors.New("the nickname is reserved")
	ErrStatic   = errors.New("the nickname is permanent")
	ErrNotYours = errors.New("the nickname belongs to another address")
	ErrNotFound = errors.New("no such nickname")
)

// migrations are applied in order; PRAGMA user_version says how many have been.
var migrations = []string{
	`CREATE TABLE nicks(
		nick_key TEXT PRIMARY KEY,
		nick     TEXT NOT NULL,
		sigil    TEXT NOT NULL,
		name_lc  TEXT NOT NULL,
		address  TEXT NOT NULL,
		static   INTEGER NOT NULL DEFAULT 0,
		created  INTEGER NOT NULL,
		updated  INTEGER NOT NULL,
		UNIQUE(address, sigil)
	);
	CREATE TABLE released(
		nick_key TEXT PRIMARY KEY,
		address  TEXT NOT NULL,
		until    INTEGER NOT NULL
	);
	CREATE TABLE reserved(
		nick_key TEXT PRIMARY KEY
	);
	CREATE TABLE handled(
		event_id TEXT PRIMARY KEY,
		at       INTEGER NOT NULL,
		reply    TEXT NOT NULL
	);
	CREATE INDEX handled_at ON handled(at);
	CREATE TABLE history(
		id      INTEGER PRIMARY KEY AUTOINCREMENT,
		at      INTEGER NOT NULL,
		address TEXT NOT NULL,
		action  TEXT NOT NULL,
		nick    TEXT NOT NULL,
		detail  TEXT NOT NULL DEFAULT ''
	);
	CREATE INDEX history_address_at ON history(address, at);
	CREATE INDEX history_at ON history(at);`,
}

// Entry is one registered nickname.
type Entry struct {
	Nick    string // as it was registered, "@Ben"
	Key     string // what makes it unique, "@ben"
	Address string
	Static  bool // a chan's name or the operator's: it never changes
	Created time.Time
	Updated time.Time
}

// Store is the registry.
type Store struct {
	db  *sql.DB
	now func() time.Time
}

// Option configures Open.
type Option func(*Store)

// WithClock replaces the clock, for tests.
func WithClock(now func() time.Time) Option { return func(s *Store) { s.now = now } }

// Open opens the file, making it and bringing its schema up to date.
func Open(path string, options ...Option) (*Store, error) {
	dsn := "file:" + (&url.URL{Path: path}).EscapedPath() +
		"?_txlock=immediate&_pragma=busy_timeout(10000)&_pragma=journal_mode(WAL)" +
		"&_pragma=synchronous(NORMAL)&_pragma=foreign_keys(1)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	// One connection: this process serialises its own work, and other processes (the admin
	// commands) wait on the file lock.
	db.SetMaxOpenConns(1)
	s := &Store{db: db, now: time.Now}
	for _, option := range options {
		option(s)
	}
	if err := s.migrate(); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

// Close closes the file.
func (s *Store) Close() error { return s.db.Close() }

func (s *Store) userVersion() (int, error) {
	var v int
	err := s.db.QueryRow("PRAGMA user_version").Scan(&v)
	return v, err
}

func (s *Store) migrate() error {
	version, err := s.userVersion()
	if err != nil {
		return err
	}
	if version > len(migrations) {
		return fmt.Errorf("the database is from a newer nickbot (schema %d, this one knows %d)", version, len(migrations))
	}
	for ; version < len(migrations); version++ {
		tx, err := s.db.BeginTx(context.Background(), nil)
		if err != nil {
			return err
		}
		if _, err := tx.Exec(migrations[version]); err != nil {
			tx.Rollback()
			return fmt.Errorf("migration %d: %w", version+1, err)
		}
		if _, err := tx.Exec(fmt.Sprintf("PRAGMA user_version = %d", version+1)); err != nil {
			tx.Rollback()
			return err
		}
		if err := tx.Commit(); err != nil {
			return err
		}
	}
	return nil
}

type querier interface {
	Exec(query string, args ...any) (sql.Result, error)
	Query(query string, args ...any) (*sql.Rows, error)
	QueryRow(query string, args ...any) *sql.Row
}

// Reg is the registry's operations. From Store.Reg each one is its own transaction; given to the
// function of Store.Do they all share the transaction of that call.
type Reg struct {
	s  *Store
	tx *sql.Tx
}

// Reg returns the operations, each in a transaction of its own.
func (s *Store) Reg() *Reg { return &Reg{s: s} }

// atomically runs f in a transaction: the caller's if there is one, else a new one.
func (r *Reg) atomically(f func(q querier) error) error {
	if r.tx != nil {
		return f(r.tx)
	}
	tx, err := r.s.db.BeginTx(context.Background(), nil)
	if err != nil {
		return err
	}
	if err := f(tx); err != nil {
		tx.Rollback()
		return err
	}
	return tx.Commit()
}

func (r *Reg) reader() querier {
	if r.tx != nil {
		return r.tx
	}
	return r.s.db
}

func unix(t time.Time) int64 { return t.Unix() }

// Do runs fn once for the event: in one transaction, which also records the event and the reply
// fn returned. If the event was handled before (the library hands an event out again after a crash)
// fn is not run and the recorded reply comes back with replayed true. An error from fn rolls it all
// back and records nothing. fn must use only the Reg it is given.
func (s *Store) Do(eventID string, fn func(*Reg) (string, error)) (reply string, replayed bool, err error) {
	tx, err := s.db.BeginTx(context.Background(), nil)
	if err != nil {
		return "", false, err
	}
	err = tx.QueryRow("SELECT reply FROM handled WHERE event_id=?", eventID).Scan(&reply)
	switch {
	case err == nil:
		tx.Rollback()
		return reply, true, nil
	case !errors.Is(err, sql.ErrNoRows):
		tx.Rollback()
		return "", false, err
	}
	reply, err = fn(&Reg{s: s, tx: tx})
	if err != nil {
		tx.Rollback()
		return "", false, err
	}
	if _, err := tx.Exec("INSERT INTO handled(event_id, at, reply) VALUES(?,?,?)", eventID, unix(s.now()), reply); err != nil {
		tx.Rollback()
		return "", false, err
	}
	return reply, false, tx.Commit()
}

func scanEntry(row interface{ Scan(...any) error }) (Entry, error) {
	var e Entry
	var static int
	var created, updated int64
	err := row.Scan(&e.Nick, &e.Key, &e.Address, &static, &created, &updated)
	e.Static = static != 0
	e.Created, e.Updated = time.Unix(created, 0), time.Unix(updated, 0)
	return e, err
}

const entryColumns = "nick, nick_key, address, static, created, updated"

// RegisterOptions say how a name is registered.
type RegisterOptions struct {
	// Static makes the name permanent. The caller has checked the right to it: a chan's name against
	// the chan's own address.
	Static bool
}

// RegisterResult says what Register did.
type RegisterResult struct {
	Entry    Entry
	Already  bool   // the address had this name already; nothing changed
	Replaced string // the name of the same sigil that this one took the place of
}

// ByNick finds a nickname's entry.
func (r *Reg) ByNick(n nick.Nick) (Entry, bool, error) {
	e, err := scanEntry(r.reader().QueryRow("SELECT "+entryColumns+" FROM nicks WHERE nick_key=?", n.Key()))
	if errors.Is(err, sql.ErrNoRows) {
		return Entry{}, false, nil
	}
	return e, err == nil, err
}

// ByAddress lists an address's names: the person's, the bot's, the chan's.
func (r *Reg) ByAddress(address string) ([]Entry, error) {
	rows, err := r.reader().Query("SELECT "+entryColumns+" FROM nicks WHERE address=? "+
		"ORDER BY CASE sigil WHEN '@' THEN 0 WHEN '/' THEN 1 ELSE 2 END", address)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var list []Entry
	for rows.Next() {
		e, err := scanEntry(rows)
		if err != nil {
			return nil, err
		}
		list = append(list, e)
	}
	return list, rows.Err()
}

// Register gives the address the name. See spec 3.2: a name another address holds is ErrTaken (also
// while it is held for that address's old owner), a reserved one ErrReserved, and an address whose
// name of that sigil is permanent ErrStatic. A name the address has already is not an error.
func (r *Reg) Register(n nick.Nick, address string, o RegisterOptions) (RegisterResult, error) {
	var result RegisterResult
	err := r.atomically(func(q querier) error {
		now := unix(r.s.now())
		key := n.Key()
		var reserved int
		if err := q.QueryRow("SELECT COUNT(*) FROM reserved WHERE nick_key=?", key).Scan(&reserved); err != nil {
			return err
		}
		if reserved > 0 {
			return ErrReserved
		}
		current, err := scanEntry(q.QueryRow("SELECT "+entryColumns+" FROM nicks WHERE nick_key=?", key))
		switch {
		case err == nil:
			if current.Address != address {
				return ErrTaken
			}
			result = RegisterResult{Entry: current, Already: true}
			return nil
		case !errors.Is(err, sql.ErrNoRows):
			return err
		}
		var holder string
		var until int64
		err = q.QueryRow("SELECT address, until FROM released WHERE nick_key=?", key).Scan(&holder, &until)
		switch {
		case err == nil:
			if holder != address && until > now {
				return ErrTaken
			}
		case !errors.Is(err, sql.ErrNoRows):
			return err
		}
		// The address's name of this sigil, if any, makes way.
		old, err := scanEntry(q.QueryRow("SELECT "+entryColumns+" FROM nicks WHERE address=? AND sigil=?", address, string(n.Sigil)))
		action := "register"
		switch {
		case err == nil:
			if old.Static {
				return ErrStatic
			}
			if _, err := q.Exec("DELETE FROM nicks WHERE nick_key=?", old.Key); err != nil {
				return err
			}
			if _, err := q.Exec("INSERT OR REPLACE INTO released(nick_key, address, until) VALUES(?,?,?)",
				old.Key, address, now+int64(Quarantine/time.Second)); err != nil {
				return err
			}
			result.Replaced = old.Nick
			action = "change"
		case !errors.Is(err, sql.ErrNoRows):
			return err
		}
		static := 0
		if o.Static {
			static = 1
		}
		if _, err := q.Exec("INSERT INTO nicks(nick_key, nick, sigil, name_lc, address, static, created, updated) VALUES(?,?,?,?,?,?,?,?)",
			key, n.String(), string(n.Sigil), n.Lower(), address, static, now, now); err != nil {
			return err
		}
		if _, err := q.Exec("DELETE FROM released WHERE nick_key=?", key); err != nil {
			return err
		}
		if err := r.record(q, now, address, action, n.String(), result.Replaced); err != nil {
			return err
		}
		result.Entry, err = scanEntry(q.QueryRow("SELECT "+entryColumns+" FROM nicks WHERE nick_key=?", key))
		return err
	})
	if err != nil {
		return RegisterResult{}, err
	}
	return result, nil
}

// Unregister takes the name from its owner and holds it for them.
func (r *Reg) Unregister(n nick.Nick, address string) error {
	return r.atomically(func(q querier) error {
		now := unix(r.s.now())
		e, err := scanEntry(q.QueryRow("SELECT "+entryColumns+" FROM nicks WHERE nick_key=?", n.Key()))
		if errors.Is(err, sql.ErrNoRows) {
			return ErrNotFound
		}
		if err != nil {
			return err
		}
		if e.Address != address {
			return ErrNotYours
		}
		if e.Static {
			return ErrStatic
		}
		if _, err := q.Exec("DELETE FROM nicks WHERE nick_key=?", e.Key); err != nil {
			return err
		}
		if _, err := q.Exec("INSERT OR REPLACE INTO released(nick_key, address, until) VALUES(?,?,?)",
			e.Key, address, now+int64(Quarantine/time.Second)); err != nil {
			return err
		}
		return r.record(q, now, address, "unregister", e.Nick, "")
	})
}

func (r *Reg) record(q querier, now int64, address, action, nickname, detail string) error {
	_, err := q.Exec("INSERT INTO history(at, address, action, nick, detail) VALUES(?,?,?,?,?)",
		now, address, action, nickname, detail)
	return err
}

// Search finds names that contain the term (a lower-case string), those that begin with it first,
// then by name, at most limit of them; more says there were others. The term is only ever a bound
// parameter and is looked for with instr(), not LIKE, so nothing in it is a wildcard.
func (r *Reg) Search(term nick.Term, limit int) (list []Entry, more bool, err error) {
	sigil := ""
	if term.Sigil != 0 {
		sigil = string(term.Sigil)
	}
	rows, err := r.reader().Query("SELECT "+entryColumns+" FROM nicks "+
		"WHERE instr(name_lc, ?1) > 0 AND (?2 = '' OR sigil = ?2) "+
		"ORDER BY (substr(name_lc, 1, length(?1)) = ?1) DESC, name_lc, nick_key LIMIT ?3",
		term.Text, sigil, limit+1)
	if err != nil {
		return nil, false, err
	}
	defer rows.Close()
	for rows.Next() {
		e, err := scanEntry(rows)
		if err != nil {
			return nil, false, err
		}
		list = append(list, e)
	}
	if err := rows.Err(); err != nil {
		return nil, false, err
	}
	if len(list) > limit {
		list, more = list[:limit], true
	}
	return list, more, nil
}

// CountChanges counts the registrations and changes of an address since the time.
func (r *Reg) CountChanges(address string, since time.Time) (int, error) {
	var n int
	err := r.reader().QueryRow("SELECT COUNT(*) FROM history WHERE address=? AND at>=? AND action IN ('register','change')",
		address, unix(since)).Scan(&n)
	return n, err
}

// CountAllChanges counts everyone's.
func (r *Reg) CountAllChanges(since time.Time) (int, error) {
	var n int
	err := r.reader().QueryRow("SELECT COUNT(*) FROM history WHERE at>=? AND action IN ('register','change')",
		unix(since)).Scan(&n)
	return n, err
}

// SeedEntry is a line of the operator's seed file.
type SeedEntry struct {
	Nick    nick.Nick
	Address string // empty: only reserve the name
}

// Seed applies the operator's file: a name with an address becomes that address's permanent name
// (the operator wins over anyone who had it), a name without one is reserved. Doing it again changes
// nothing.
func (s *Store) Seed(entries []SeedEntry) error {
	r := &Reg{s: s}
	return r.atomically(func(q querier) error {
		now := unix(s.now())
		for _, e := range entries {
			key := e.Nick.Key()
			if e.Address == "" {
				if err := r.reserve(q, now, e.Nick); err != nil {
					return err
				}
				continue
			}
			if _, err := q.Exec("DELETE FROM reserved WHERE nick_key=?", key); err != nil {
				return err
			}
			current, err := scanEntry(q.QueryRow("SELECT "+entryColumns+" FROM nicks WHERE nick_key=?", key))
			if err == nil && current.Address == e.Address && current.Static && current.Nick == e.Nick.String() {
				continue
			}
			if err != nil && !errors.Is(err, sql.ErrNoRows) {
				return err
			}
			// Whatever stood in the way goes: the name's holder and the address's name of this sigil.
			if _, err := q.Exec("DELETE FROM nicks WHERE nick_key=? OR (address=? AND sigil=?)", key, e.Address, string(e.Nick.Sigil)); err != nil {
				return err
			}
			if _, err := q.Exec("DELETE FROM released WHERE nick_key=?", key); err != nil {
				return err
			}
			if _, err := q.Exec("INSERT INTO nicks(nick_key, nick, sigil, name_lc, address, static, created, updated) VALUES(?,?,?,?,?,1,?,?)",
				key, e.Nick.String(), string(e.Nick.Sigil), e.Nick.Lower(), e.Address, now, now); err != nil {
				return err
			}
			if err := r.record(q, now, e.Address, "seed", e.Nick.String(), ""); err != nil {
				return err
			}
		}
		return nil
	})
}

func (r *Reg) reserve(q querier, now int64, n nick.Nick) error {
	var holder string
	err := q.QueryRow("SELECT address FROM nicks WHERE nick_key=?", n.Key()).Scan(&holder)
	switch {
	case err == nil:
		if _, err := q.Exec("DELETE FROM nicks WHERE nick_key=?", n.Key()); err != nil {
			return err
		}
		if err := r.record(q, now, holder, "reserve", n.String(), "removed by the operator"); err != nil {
			return err
		}
	case !errors.Is(err, sql.ErrNoRows):
		return err
	}
	if _, err := q.Exec("DELETE FROM released WHERE nick_key=?", n.Key()); err != nil {
		return err
	}
	_, err = q.Exec("INSERT OR IGNORE INTO reserved(nick_key) VALUES(?)", n.Key())
	return err
}

// Reserve is the operator's: the name is removed if somebody has it, and nobody can register it.
func (s *Store) Reserve(n nick.Nick) error {
	r := &Reg{s: s}
	return r.atomically(func(q querier) error { return r.reserve(q, unix(s.now()), n) })
}

// Remove is the operator's: the name is deleted, static or not, and is free at once.
func (s *Store) Remove(n nick.Nick) error {
	r := &Reg{s: s}
	return r.atomically(func(q querier) error {
		now := unix(s.now())
		var holder string
		err := q.QueryRow("SELECT address FROM nicks WHERE nick_key=?", n.Key()).Scan(&holder)
		if errors.Is(err, sql.ErrNoRows) {
			return ErrNotFound
		}
		if err != nil {
			return err
		}
		if _, err := q.Exec("DELETE FROM nicks WHERE nick_key=?", n.Key()); err != nil {
			return err
		}
		return r.record(q, now, holder, "remove", n.String(), "by the operator")
	})
}

// HistoryRow is a line of the history.
type HistoryRow struct {
	At      time.Time
	Address string
	Action  string
	Nick    string
	Detail  string
}

// History lists an address's history, newest first.
func (s *Store) History(address string, limit int) ([]HistoryRow, error) {
	rows, err := s.db.Query("SELECT at, address, action, nick, detail FROM history WHERE address=? ORDER BY id DESC LIMIT ?", address, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var list []HistoryRow
	for rows.Next() {
		var h HistoryRow
		var at int64
		if err := rows.Scan(&at, &h.Address, &h.Action, &h.Nick, &h.Detail); err != nil {
			return nil, err
		}
		h.At = time.Unix(at, 0)
		list = append(list, h)
	}
	return list, rows.Err()
}

// Stats counts the registry.
type Stats struct {
	Total, People, Bots, Chans, Addresses, Held, Reserved int
}

// Stats counts what is registered.
func (s *Store) Stats() (Stats, error) {
	var st Stats
	err := s.db.QueryRow(`SELECT COUNT(*),
		COALESCE(SUM(sigil='@'),0), COALESCE(SUM(sigil='/'),0), COALESCE(SUM(sigil='#'),0),
		COUNT(DISTINCT address) FROM nicks`).Scan(&st.Total, &st.People, &st.Bots, &st.Chans, &st.Addresses)
	if err != nil {
		return st, err
	}
	if err := s.db.QueryRow("SELECT COUNT(*) FROM released WHERE until>?", unix(s.now())).Scan(&st.Held); err != nil {
		return st, err
	}
	err = s.db.QueryRow("SELECT COUNT(*) FROM reserved").Scan(&st.Reserved)
	return st, err
}

// Export lists every entry in the order of its key.
func (s *Store) Export() ([]Entry, error) {
	rows, err := s.db.Query("SELECT " + entryColumns + " FROM nicks ORDER BY nick_key")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var list []Entry
	for rows.Next() {
		e, err := scanEntry(rows)
		if err != nil {
			return nil, err
		}
		list = append(list, e)
	}
	return list, rows.Err()
}

// Prune forgets old handled events, ended holds and old history.
func (s *Store) Prune() error {
	r := &Reg{s: s}
	return r.atomically(func(q querier) error {
		now := s.now()
		for _, stmt := range []struct {
			sql string
			arg int64
		}{
			{"DELETE FROM handled WHERE at<?", unix(now.Add(-HandledKeep))},
			{"DELETE FROM released WHERE until<=?", unix(now)},
			{"DELETE FROM history WHERE at<?", unix(now.Add(-HistoryKeep))},
		} {
			if _, err := q.Exec(stmt.sql, stmt.arg); err != nil {
				return err
			}
		}
		return nil
	})
}

// Backup writes a consistent copy of the database to a new file (an existing one is an error).
func (s *Store) Backup(path string) error {
	quoted := "'" + strings.ReplaceAll(path, "'", "''") + "'"
	_, err := s.db.Exec("VACUUM INTO " + quoted)
	return err
}

// Total is the number of names registered.
func (r *Reg) Total() (int, error) {
	var n int
	err := r.reader().QueryRow("SELECT COUNT(*) FROM nicks").Scan(&n)
	return n, err
}
