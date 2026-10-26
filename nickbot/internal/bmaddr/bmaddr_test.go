package bmaddr

import (
	"crypto/sha512"
	"math/big"
	"strings"
	"testing"
)

// Addresses the C++ core and PyBitmessage agree on (tests/bmclient/protocol_test.cpp, updates.cpp).
var known = []struct {
	address string
	version byte
}{
	{"BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr", 3},  // the chan "general", version 3
	{"BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r", 4},  // the chan "general", version 4
	{"BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN", 4},  // one leading zero byte in its RIPE
	{"BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6", 4},    // two
	{"BM-2cUqyPSNiBCvTg51hztE4uWrxET3386Ezo", 4},  // a phrase of non-ASCII letters
	{"BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8", 4}, // the echo bot: a full 20-byte RIPE
	{"BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9", 4},     // the Bitmessage digest
}

// rawEncode builds an address string from the pieces, trimming nothing, so that a test can make
// addresses with a good checksum that the codec would never write.
func rawEncode(version, stream byte, ripe []byte) string {
	body := append([]byte{version, stream}, ripe...)
	first := sha512.Sum512(body)
	second := sha512.Sum512(first[:])
	body = append(body, second[:4]...)
	n := new(big.Int).SetBytes(body)
	var out []byte
	m := new(big.Int)
	base := big.NewInt(58)
	for n.Sign() > 0 {
		n.DivMod(n, base, m)
		out = append(out, alphabet[m.Int64()])
	}
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return "BM-" + string(out)
}

func TestKnownAddressesAreValid(t *testing.T) {
	for _, k := range known {
		a, err := Parse(k.address)
		if err != nil {
			t.Errorf("Parse(%q): %v", k.address, err)
			continue
		}
		if a.Version != k.version || a.Stream != 1 {
			t.Errorf("Parse(%q) = version %d stream %d, want %d and 1", k.address, a.Version, a.Stream, k.version)
		}
		if a.String() != k.address {
			t.Errorf("%q came back as %q", k.address, a.String())
		}
		if !Valid(k.address) {
			t.Errorf("Valid(%q) = false", k.address)
		}
	}
}

func TestRipeIsRightAligned(t *testing.T) {
	a, err := Parse("BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6") // two zero bytes trimmed
	if err != nil {
		t.Fatal(err)
	}
	if a.Ripe[0] != 0 || a.Ripe[1] != 0 {
		t.Errorf("the two trimmed bytes are zero, got % x", a.Ripe[:4])
	}
	if a.Ripe[2] == 0 && a.Ripe[3] == 0 {
		t.Errorf("the rest is not all zero, got % x", a.Ripe[:4])
	}
}

func TestRejects(t *testing.T) {
	good := known[1].address
	for name, bad := range map[string]string{
		"empty":             "",
		"prefix only":       "BM-",
		"no prefix":         good[3:],
		"lower-case prefix": "bm-" + good[3:],
		"other prefix":      "BX-" + good[3:],
		"surrounding space": " " + good,
		"trailing space":    good + " ",
		"trailing newline":  good + "\n",
		"nul":               good + "\x00",
		"zero":              strings.Replace(good, "2", "0", 1),
		"capital O":         strings.Replace(good, "W", "O", 1),
		"capital I":         strings.Replace(good, "G", "I", 1),
		"small l":           strings.Replace(good, "k", "l", 1),
		"too long":          good + strings.Repeat("2", 40),
		"unicode":           good[:10] + "é" + good[11:],
		"nonsense":          "hello world",
	} {
		if Valid(bad) {
			t.Errorf("%s: %q was accepted", name, bad)
		}
		if _, err := Parse(bad); err == nil {
			t.Errorf("%s: Parse(%q) had no error", name, bad)
		}
	}
}

func TestEveryCharacterOfTheChecksumCounts(t *testing.T) {
	good := known[1].address
	for i := 3; i < len(good); i++ { // the prefix is checked on its own
		for _, c := range []byte("12AzZ") {
			if good[i] == c {
				continue
			}
			bad := good[:i] + string(c) + good[i+1:]
			if Valid(bad) {
				t.Errorf("a change at %d to %q still passes: %s", i, c, bad)
			}
		}
	}
}

func TestVersionAndStreamRules(t *testing.T) {
	ripe := make([]byte, 20)
	for i := range ripe {
		ripe[i] = byte(i + 1)
	}
	for _, tc := range []struct {
		version, stream byte
		ok              bool
	}{
		{1, 1, false},
		{2, 1, true},
		{3, 1, true},
		{4, 1, true},
		{5, 1, false},
		{4, 0, false},
		{4, 2, false},
		{4, 255, false},
	} {
		s := rawEncode(tc.version, tc.stream, ripe)
		if got := Valid(s); got != tc.ok {
			t.Errorf("version %d stream %d: Valid = %v, want %v (%s)", tc.version, tc.stream, got, tc.ok, s)
		}
	}
}

func TestTheRipeIsTrimmedTheWayTheCodecWritesIt(t *testing.T) {
	zeros := func(n int) []byte {
		ripe := make([]byte, 20)
		for i := n; i < 20; i++ {
			ripe[i] = byte(i + 1)
		}
		return ripe
	}
	// A version 4 address drops every leading zero byte, a version 3 address at most two.
	for _, tc := range []struct {
		version byte
		zeros   int
		trim    int
		ok      bool
	}{
		{4, 1, 1, true},
		{4, 1, 0, false}, // not trimmed
		{4, 3, 3, true},
		{4, 3, 2, false},
		{3, 2, 2, true},
		{3, 3, 2, true}, // a third zero byte stays in a version 3 address
		{3, 3, 3, false},
		{3, 1, 0, false},
	} {
		s := rawEncode(tc.version, 1, zeros(tc.zeros)[tc.trim:])
		if got := Valid(s); got != tc.ok {
			t.Errorf("version %d, %d zero bytes, %d trimmed: Valid = %v, want %v", tc.version, tc.zeros, tc.trim, got, tc.ok)
		}
	}
}

func TestAnAllZeroRipeIsValidOnlyWhenFullyTrimmed(t *testing.T) {
	// A version 4 address drops every leading zero byte, so a RIPE of zeros is written with none.
	if !Valid(rawEncode(4, 1, nil)) {
		t.Error("an all-zero RIPE, fully trimmed, is what the codec writes")
	}
	for n := 1; n <= 20; n++ {
		if s := rawEncode(4, 1, make([]byte, n)); Valid(s) {
			t.Errorf("an untrimmed all-zero RIPE of %d bytes was accepted", n)
		}
	}
}

func TestEncodeThenParse(t *testing.T) {
	var a Address
	a.Version, a.Stream = 4, 1
	for i := range a.Ripe {
		a.Ripe[i] = byte(0xA0 + i)
	}
	back, err := Parse(a.String())
	if err != nil || back != a {
		t.Errorf("round trip: %+v, %v", back, err)
	}
	a.Ripe[0] = 0
	back, err = Parse(a.String())
	if err != nil || back != a {
		t.Errorf("round trip with a zero byte: %+v, %v", back, err)
	}
}
