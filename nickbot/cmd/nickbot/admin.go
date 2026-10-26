package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/nick"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

const adminUsage = "usage: nickbot admin -db FILE stats | export | remove NICK | reserve NICK | history ADDRESS"

// runAdmin is the operator's side of the registry: it works on the database file while the service
// runs (docker exec), and prints what it finds to out.
func runAdmin(args []string, out io.Writer) error {
	fs := flag.NewFlagSet("admin", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	db := fs.String("db", "", "the registry file")
	if err := fs.Parse(args); err != nil {
		return fmt.Errorf("%v\n%s", err, adminUsage)
	}
	rest := fs.Args()
	if *db == "" || len(rest) == 0 {
		return errors.New(adminUsage)
	}
	if _, err := os.Stat(*db); err != nil {
		return fmt.Errorf("no registry at %s", *db)
	}
	st, err := store.Open(*db)
	if err != nil {
		return err
	}
	defer st.Close()
	parse := func() (nick.Nick, error) {
		if len(rest) != 2 {
			return nick.Nick{}, errors.New(adminUsage)
		}
		n, err := nick.Parse(rest[1])
		if err != nil {
			return nick.Nick{}, fmt.Errorf("%q is not a nickname", rest[1])
		}
		return n, nil
	}
	switch rest[0] {
	case "stats":
		s, err := st.Stats()
		if err != nil {
			return err
		}
		fmt.Fprintf(out, "names: %d\npeople: %d\nbots: %d\nchans: %d\naddresses: %d\nheld: %d\nreserved: %d\n",
			s.Total, s.People, s.Bots, s.Chans, s.Addresses, s.Held, s.Reserved)
	case "export":
		list, err := st.Export()
		if err != nil {
			return err
		}
		for _, e := range list {
			static := "no"
			if e.Static {
				static = "yes"
			}
			fmt.Fprintf(out, "%s\t%s\t%s\n", e.Nick, e.Address, static)
		}
	case "remove":
		n, err := parse()
		if err != nil {
			return err
		}
		return st.Remove(n)
	case "reserve":
		n, err := parse()
		if err != nil {
			return err
		}
		return st.Reserve(n)
	case "history":
		if len(rest) != 2 {
			return errors.New(adminUsage)
		}
		rows, err := st.History(rest[1], 100)
		if err != nil {
			return err
		}
		for _, h := range rows {
			fmt.Fprintf(out, "%s\t%s\t%s\t%s\n", h.At.UTC().Format(time.RFC3339), h.Action, h.Nick, h.Detail)
		}
	default:
		return errors.New(adminUsage)
	}
	return nil
}
