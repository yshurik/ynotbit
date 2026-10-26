// Package bmaddr reads and writes Bitmessage addresses the way the C++ core does
// (bmc::Wire::address): "BM-" and the base 58 of version, stream, the RIPE hash with its leading
// zero bytes trimmed, and four bytes of checksum. An address is valid only in the exact form the
// codec writes, so one address has one spelling.
package bmaddr

import (
	"crypto/sha512"
	"errors"
	"math/big"
	"strings"
)

const alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

const (
	prefix     = "BM-"
	maxLength  = 64
	ripeLength = 20
)

// ErrInvalid: not a Bitmessage address of a version and stream ynotbit uses.
var ErrInvalid = errors.New("not a valid Bitmessage address")

// Address is what an address string says.
type Address struct {
	Version, Stream byte
	Ripe            [ripeLength]byte
}

func checksum(body []byte) [4]byte {
	first := sha512.Sum512(body)
	second := sha512.Sum512(first[:])
	var out [4]byte
	copy(out[:], second[:4])
	return out
}

// String is the address as the codec writes it.
func (a Address) String() string {
	maxTrim := 2
	if a.Version >= 4 {
		maxTrim = ripeLength
	}
	ripe := a.Ripe[:]
	for i := 0; i < maxTrim && len(ripe) > 0 && ripe[0] == 0; i++ {
		ripe = ripe[1:]
	}
	body := append([]byte{a.Version, a.Stream}, ripe...)
	sum := checksum(body)
	body = append(body, sum[:]...)
	n := new(big.Int).SetBytes(body)
	var digits []byte
	base, rest := big.NewInt(58), new(big.Int)
	for n.Sign() > 0 {
		n.DivMod(n, base, rest)
		digits = append(digits, alphabet[rest.Int64()])
	}
	for i, j := 0, len(digits)-1; i < j; i, j = i+1, j-1 {
		digits[i], digits[j] = digits[j], digits[i]
	}
	return prefix + string(digits)
}

// Parse reads an address, trimming nothing. It accepts versions 2 to 4 in stream 1, and only the
// spelling String gives back.
func Parse(s string) (Address, error) {
	if len(s) > maxLength || !strings.HasPrefix(s, prefix) {
		return Address{}, ErrInvalid
	}
	n := new(big.Int)
	base := big.NewInt(58)
	for i := len(prefix); i < len(s); i++ {
		digit := strings.IndexByte(alphabet, s[i])
		if digit < 0 {
			return Address{}, ErrInvalid
		}
		n.Mul(n, base)
		n.Add(n, big.NewInt(int64(digit)))
	}
	raw := n.Bytes()
	if len(raw) < 2+4 || len(raw) > 2+ripeLength+4 {
		return Address{}, ErrInvalid
	}
	body, sum := raw[:len(raw)-4], raw[len(raw)-4:]
	want := checksum(body)
	if string(sum) != string(want[:]) {
		return Address{}, ErrInvalid
	}
	a := Address{Version: body[0], Stream: body[1]}
	ripe := body[2:]
	copy(a.Ripe[ripeLength-len(ripe):], ripe)
	if a.Version < 2 || a.Version > 4 || a.Stream != 1 || a.String() != s {
		return Address{}, ErrInvalid
	}
	return a, nil
}

// Valid says whether s is an address.
func Valid(s string) bool {
	_, err := Parse(s)
	return err == nil
}
