package limit

import (
	"fmt"
	"sync"
	"testing"
	"time"
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) advance(d time.Duration) { c.t = c.t.Add(d) }

func newClock() *clock { return &clock{t: time.Unix(1_800_000_000, 0)} }

func TestABurstThenARefill(t *testing.T) {
	c := newClock()
	l := New(Config{Burst: 3, Every: 10 * time.Second}, 100, c.now)
	for i := 0; i < 3; i++ {
		if !l.Allow("a") {
			t.Fatalf("call %d of the burst was refused", i+1)
		}
	}
	if l.Allow("a") {
		t.Error("the fourth call went through")
	}
	c.advance(9 * time.Second)
	if l.Allow("a") {
		t.Error("a token came 1 second early")
	}
	c.advance(1 * time.Second)
	if !l.Allow("a") {
		t.Error("no token after the interval")
	}
	if l.Allow("a") {
		t.Error("one interval gave two tokens")
	}
}

func TestTokensDoNotPileUpBeyondTheBurst(t *testing.T) {
	c := newClock()
	l := New(Config{Burst: 2, Every: time.Second}, 100, c.now)
	if !l.Allow("a") { // the bucket exists from here on, and one token is spent
		t.Fatal("the first call was refused")
	}
	c.advance(time.Hour)
	if !l.Allow("a") || !l.Allow("a") || l.Allow("a") {
		t.Error("after a long idle the bucket holds exactly its burst, not an hour of tokens")
	}
}

func TestKeysAreIndependent(t *testing.T) {
	c := newClock()
	l := New(Config{Burst: 1, Every: time.Minute}, 100, c.now)
	if !l.Allow("a") || l.Allow("a") {
		t.Fatal("a: one call, then none")
	}
	if !l.Allow("b") {
		t.Error("b was refused because a used its token")
	}
}

func TestPartialIntervalsAddUp(t *testing.T) {
	c := newClock()
	l := New(Config{Burst: 1, Every: 10 * time.Second}, 100, c.now)
	l.Allow("a")
	for i := 0; i < 3; i++ {
		c.advance(3 * time.Second)
		if l.Allow("a") {
			t.Fatalf("a token after only %d seconds", 3*(i+1))
		}
	}
	c.advance(1 * time.Second) // 10 seconds in all
	if !l.Allow("a") {
		t.Error("the pieces of the interval did not add up")
	}
}

func TestTheLeastRecentKeyIsForgottenWhenThereAreTooMany(t *testing.T) {
	c := newClock()
	l := New(Config{Burst: 1, Every: time.Hour}, 3, c.now)
	for _, k := range []string{"a", "b", "c"} {
		l.Allow(k) // all three use their token
	}
	l.Allow("a") // a is the most recent now
	l.Allow("d") // a fourth key: b, the least recent, goes
	if l.Len() != 3 {
		t.Fatalf("%d keys are held, want 3", l.Len())
	}
	if l.Allow("a") {
		t.Error("a was forgotten though it was used last")
	}
	if !l.Allow("b") {
		t.Error("b should have been forgotten and so given a fresh bucket")
	}
}

func TestMemoryStaysBoundedForAFloodOfKeys(t *testing.T) {
	c := newClock()
	l := New(Config{Burst: 1, Every: time.Hour}, 50, c.now)
	for i := 0; i < 10_000; i++ {
		l.Allow(fmt.Sprintf("sender-%d", i))
	}
	if l.Len() != 50 {
		t.Errorf("%d keys held, want 50", l.Len())
	}
}

func TestOnePerMinuteNotice(t *testing.T) {
	c := newClock()
	l := New(Config{Burst: 1, Every: time.Minute}, 100, c.now)
	if !l.Allow("a") {
		t.Fatal("the first notice was refused")
	}
	for i := 1; i <= 5; i++ {
		c.advance(10 * time.Second)
		if l.Allow("a") {
			t.Fatalf("a second notice after %d seconds", 10*i)
		}
	}
	c.advance(10 * time.Second)
	if !l.Allow("a") {
		t.Error("no notice after the minute")
	}
}

func TestConcurrentUse(t *testing.T) {
	l := New(Config{Burst: 100, Every: time.Hour}, 10, time.Now)
	var wg sync.WaitGroup
	allowed := make(chan bool, 1000)
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 100; j++ {
				allowed <- l.Allow("shared")
			}
		}()
	}
	wg.Wait()
	close(allowed)
	n := 0
	for ok := range allowed {
		if ok {
			n++
		}
	}
	if n != 100 {
		t.Errorf("%d calls allowed, want exactly the burst of 100", n)
	}
}

func TestABadConfigIsAProgrammingError(t *testing.T) {
	for _, cfg := range []Config{{Burst: 0, Every: time.Second}, {Burst: 1, Every: 0}, {Burst: -1, Every: time.Second}} {
		func() {
			defer func() {
				if recover() == nil {
					t.Errorf("New(%+v) did not panic", cfg)
				}
			}()
			New(cfg, 10, time.Now)
		}()
	}
}
