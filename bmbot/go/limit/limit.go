// Package limit holds token buckets, one per key, in memory that cannot grow without bound.
package limit

import (
	"container/list"
	"sync"
	"time"
)

// Config says how fast a key's bucket fills: one token every Every, up to Burst, and a new key starts
// full.
type Config struct {
	Burst int
	Every time.Duration
}

// A bucket counts its credit in time, not in tokens: a token costs Every, so nothing is rounded
// and the pieces of an interval add up exactly.
type bucket struct {
	key    string
	credit time.Duration
	last   time.Time
}

// Limiter is a set of token buckets. It remembers at most maxKeys of them, the most recently used:
// a sender that is forgotten starts again with a full bucket, which is the cheaper mistake than a
// map that a flood of addresses can grow.
type Limiter struct {
	mu      sync.Mutex
	cfg     Config
	maxKeys int
	now     func() time.Time
	keys    map[string]*list.Element // of *bucket, front is the most recent
	order   *list.List
}

// New makes a limiter. A bad configuration is a programming error and panics.
func New(cfg Config, maxKeys int, now func() time.Time) *Limiter {
	if cfg.Burst < 1 || cfg.Every <= 0 || maxKeys < 1 {
		panic("limit: bad configuration")
	}
	return &Limiter{cfg: cfg, maxKeys: maxKeys, now: now, keys: map[string]*list.Element{}, order: list.New()}
}

// Allow takes a token from the key's bucket and says whether there was one.
func (l *Limiter) Allow(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := l.now()
	var b *bucket
	if el, ok := l.keys[key]; ok {
		l.order.MoveToFront(el)
		b = el.Value.(*bucket)
		if elapsed := now.Sub(b.last); elapsed > 0 {
			b.credit += elapsed
		}
		if full := time.Duration(l.cfg.Burst) * l.cfg.Every; b.credit > full {
			b.credit = full
		}
		b.last = now
	} else {
		b = &bucket{key: key, credit: time.Duration(l.cfg.Burst) * l.cfg.Every, last: now}
		l.keys[key] = l.order.PushFront(b)
		for l.order.Len() > l.maxKeys {
			oldest := l.order.Back()
			delete(l.keys, oldest.Value.(*bucket).key)
			l.order.Remove(oldest)
		}
	}
	if b.credit < l.cfg.Every {
		return false
	}
	b.credit -= l.cfg.Every
	return true
}

// Len is the number of keys held.
func (l *Limiter) Len() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.order.Len()
}
