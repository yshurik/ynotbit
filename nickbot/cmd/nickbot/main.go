// Command nickbot is a Bitmessage service that maps nicknames to addresses: a bot of its own
// address, answering chats and letters with commands (/register, /address, /nickname, /search ...).
// See docs/nickbot.md.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/bmbot"
	"github.com/yshurik/ynotbit/nickbot/internal/loop"
	"github.com/yshurik/ynotbit/nickbot/internal/service"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

const usage = `nickbot: nicknames for Bitmessage addresses

  nickbot [flags]                       run the service
  nickbot --check FILE [--max-age 90s] [--peers 1]
                                        exit 0 when the status file is fresh and has peers (a health check)
  nickbot admin -db FILE COMMAND        stats | export | remove NICK | reserve NICK | history ADDRESS

Flags of the service:
`

type options struct {
	dataDir, phraseFile, keysFile, seedFile, statusFile string
	peer, proxy                                         string
	anonPhrase                                          string
	public, listen                                      bool
	port, retentionMB, retentionDays                    int
	maxOutgoing, maxIncoming                            int
	hang                                                time.Duration
	changesPerDay, newPerHour, maxNicks                 int
	check                                               string
	maxAge                                              time.Duration
	minPeers                                            int
}

func parseFlags(args []string, stderr io.Writer) (*options, error) {
	o := &options{}
	fs := flag.NewFlagSet("nickbot", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() {
		fmt.Fprint(stderr, usage)
		fs.PrintDefaults()
	}
	fs.StringVar(&o.dataDir, "data-dir", "/data", "where everything is kept: state/ (the bot), nickbot.sqlite, backup/, status.json")
	fs.StringVar(&o.phraseFile, "phrase-file", "/run/secrets/phrase", "a file whose first line is the secret phrase the address comes from")
	fs.StringVar(&o.keysFile, "keys-file", "", "instead of --phrase-file: a file with the two private keys the address is made of (signing, then encryption, one to a line; hex or wallet import format)")
	fs.StringVar(&o.seedFile, "seed", "", "the operator's file of reserved and permanent names (optional)")
	fs.StringVar(&o.statusFile, "status-file", "", "where to write the status every 10 seconds (default DATA/status.json)")
	fs.StringVar(&o.anonPhrase, "anon-phrase", "anonymous", "the phrase of the chan every lookup comes from")
	fs.BoolVar(&o.public, "public", true, "join the public Bitmessage network (false: link to --peer alone)")
	fs.StringVar(&o.peer, "peer", "", "a peer to link to, IP:port")
	fs.StringVar(&o.proxy, "proxy", "", "a SOCKS5 proxy, IP:port")
	fs.BoolVar(&o.listen, "listen", false, "accept incoming connections")
	fs.IntVar(&o.port, "port", 0, "the port to listen on (8444)")
	fs.IntVar(&o.maxOutgoing, "max-outgoing", 12, "outgoing connections")
	fs.IntVar(&o.maxIncoming, "max-incoming", 24, "incoming connections")
	fs.IntVar(&o.retentionMB, "retention-mb", 512, "how much of the network's objects the node keeps")
	fs.IntVar(&o.retentionDays, "retention-days", 4, "how long it keeps them")
	fs.DurationVar(&o.hang, "hang", 120*time.Second, "abort when the library's loop stalls this long, so that Docker starts it again (0: never)")
	fs.IntVar(&o.changesPerDay, "changes-per-day", 5, "names one address may register or change in 24 hours")
	fs.IntVar(&o.newPerHour, "new-per-hour", 200, "registrations and changes the registry takes in an hour")
	fs.IntVar(&o.maxNicks, "max-nicks", 1_000_000, "names the registry holds")
	fs.StringVar(&o.check, "check", "", "health check: the status file to read, then exit")
	fs.DurationVar(&o.maxAge, "max-age", 90*time.Second, "with --check: how old the status file may be")
	fs.IntVar(&o.minPeers, "peers", 1, "with --check: how many peers it must name")
	if err := fs.Parse(args); err != nil {
		return nil, err
	}
	if o.keysFile != "" {
		// The phrase file has a default; only one that was asked for is a second source.
		asked := false
		fs.Visit(func(f *flag.Flag) { asked = asked || f.Name == "phrase-file" })
		if asked {
			fmt.Fprintln(stderr, "nickbot: give --phrase-file or --keys-file, not both")
			return nil, errors.New("both a phrase file and a keys file")
		}
		o.phraseFile = ""
	}
	if o.statusFile == "" {
		o.statusFile = filepath.Join(o.dataDir, "status.json")
	}
	return o, nil
}

func main() { os.Exit(run(os.Args[1:], os.Stdout, os.Stderr)) }

func run(args []string, stdout, stderr io.Writer) int {
	if len(args) > 0 && args[0] == "admin" {
		if err := runAdmin(args[1:], stdout); err != nil {
			fmt.Fprintln(stderr, "nickbot admin:", err)
			return 1
		}
		return 0
	}
	o, err := parseFlags(args, stderr)
	if err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if o.check != "" {
		if err := checkStatus(o.check, o.maxAge, o.minPeers, time.Now()); err != nil {
			fmt.Fprintln(stderr, "nickbot:", err)
			return 1
		}
		return 0
	}
	logger := log.New(stdout, "", log.LstdFlags|log.LUTC)
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := serve(ctx, o, logger); err != nil {
		logger.Printf("nickbot: %v", err)
		return 1
	}
	logger.Print("Bye.")
	return 0
}

func serve(ctx context.Context, o *options, logger *log.Logger) error {
	stateDir := filepath.Join(o.dataDir, "state")
	if err := os.MkdirAll(o.dataDir, 0o700); err != nil {
		return err
	}
	st, err := store.Open(filepath.Join(o.dataDir, "nickbot.sqlite"))
	if err != nil {
		return err
	}
	defer st.Close()
	if o.seedFile != "" {
		f, err := os.Open(o.seedFile)
		if err != nil {
			return fmt.Errorf("seed file: %w", err)
		}
		entries, err := parseSeed(f)
		f.Close()
		if err != nil {
			return fmt.Errorf("seed file %s: %w", o.seedFile, err)
		}
		if err := st.Seed(entries); err != nil {
			return err
		}
		logger.Printf("seed: %d names from %s", len(entries), o.seedFile)
	}
	anonymous, err := bmbot.ChanAddress(o.anonPhrase)
	if err != nil {
		return fmt.Errorf("the lookup chan: %w", err)
	}
	bot, err := bmbot.Open(bmbot.Config{
		DataDir:     stateDir,
		PhraseFile:  o.phraseFile,
		KeysFile:    o.keysFile,
		Label:       "nickbot",
		HangSeconds: int(o.hang / time.Second),
		Node: bmbot.Node{
			Bin: "auto", Peer: o.peer, Proxy: o.proxy, Public: o.public, Listen: o.listen, Port: o.port,
			MaxOutgoing: o.maxOutgoing, MaxIncoming: o.maxIncoming,
			WatchdogMinutes: 15, RetentionMB: o.retentionMB, RetentionDays: o.retentionDays,
		},
	})
	if err != nil {
		return err
	}
	var closeOnce sync.Once
	closeBot := func() { closeOnce.Do(bot.Close) }
	defer closeBot()
	status, err := bot.Status()
	if err != nil {
		return err
	}
	logger.Printf("Address: %s", status.Address)
	logger.Printf("Lookups come from the chan %q, %s", o.anonPhrase, anonymous)

	svc := service.New(st, service.Options{
		Anonymous:     anonymous,
		ChanAddress:   bmbot.ChanAddress,
		ChangesPerDay: o.changesPerDay,
		NewPerHour:    o.newPerHour,
		MaxNicks:      o.maxNicks,
		Log:           logger.Printf,
	})
	lp := loop.New(bot, svc, loop.Config{Log: logger.Printf})

	started := time.Now()
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		writeStatusEvery(ctx, 10*time.Second, o.statusFile, bot, lp, st, anonymous, started, logger)
	}()
	go func() { defer wg.Done(); maintain(ctx, st, filepath.Join(o.dataDir, "backup"), 7, logger) }()

	err = lp.Run(ctx)
	stop := ctx.Err() != nil
	wg.Wait()
	if err != nil && !stop {
		return err
	}
	return nil
}

// writeStatusEvery writes the status file at once and then every interval, until the context ends.
func writeStatusEvery(ctx context.Context, interval time.Duration, path string, bot *bmbot.Bot, lp *loop.Loop,
	st *store.Store, anonymous string, started time.Time, logger *log.Logger) {
	write := func() {
		s, err := bot.Status()
		if err != nil {
			if ctx.Err() == nil {
				logger.Printf("status: %v", err)
			}
			return
		}
		names := 0
		if stats, err := st.Stats(); err == nil {
			names = stats.Total
		}
		c := lp.Stats()
		doc := statusFile{
			Time: time.Now().Unix(), Uptime: int64(time.Since(started).Seconds()), Address: s.Address,
			Anonymous: anonymous, Peers: s.Node.Peers, NodeRunning: s.Node.Running, Names: names,
			Received: c.Received, Chats: c.Chats, Letters: c.Letters, Answered: c.Answered,
			Limited: c.Limited, Notices: c.Notices, Dropped: c.Dropped, Errors: c.Errors,
		}
		if err := writeStatus(path, doc); err != nil {
			logger.Printf("status: %v", err)
		}
	}
	write()
	t := time.NewTicker(interval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			write()
		}
	}
}

// maintain prunes the registry and takes a snapshot of it at once and then every day, keeping
// the newest keep snapshots.
func maintain(ctx context.Context, st *store.Store, dir string, keep int, logger *log.Logger) {
	do := func() {
		if err := st.Prune(); err != nil {
			logger.Printf("prune: %v", err)
		}
		if err := snapshot(st, dir, keep, time.Now()); err != nil {
			logger.Printf("backup: %v", err)
		}
	}
	do()
	t := time.NewTicker(24 * time.Hour)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			do()
		}
	}
}

// snapshot writes the day's copy of the registry (once a day) and removes the oldest beyond keep.
func snapshot(st *store.Store, dir string, keep int, now time.Time) error {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	target := filepath.Join(dir, "nickbot-"+now.UTC().Format("20060102")+".sqlite")
	if _, err := os.Stat(target); errors.Is(err, os.ErrNotExist) {
		if err := st.Backup(target); err != nil {
			return err
		}
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	var names []string
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), "nickbot-") && strings.HasSuffix(e.Name(), ".sqlite") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	for len(names) > keep {
		if err := os.Remove(filepath.Join(dir, names[0])); err != nil {
			return err
		}
		names = names[1:]
	}
	return nil
}
