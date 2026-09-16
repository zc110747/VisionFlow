// Package netiface enumerates the machine's IPv4 addresses so the server can
// (a) bind once and be reachable on every local address, and (b) print the
// concrete URLs a user on this box - or another machine on the LAN - can open.
//
// Only IPv4 is reported: that is what the LAN clients, VLC/ffplay and the
// RTSP URLs in the UI actually use. IPv6 is deliberately ignored to keep the
// printed list short and unambiguous.
package netiface

import (
	"net"
	"sort"
	"strings"
)

// Address is one usable local IPv4 address together with the metadata needed to
// rank it (private vs public, physical vs virtual NIC) and to label it in logs.
type Address struct {
	IP        string `json:"ip"`
	Interface string `json:"interface"`
	Loopback  bool   `json:"loopback"`
	Private   bool   `json:"private"`
	LinkLocal bool   `json:"link_local"`
	Virtual   bool   `json:"virtual"`
}

// virtualKeywords matches NIC names that are not real LAN adapters. Their
// addresses are technically bound, and reachable from the host itself, but a
// user copy-pasting one into another machine's browser will get nowhere, so
// they are listed separately (and never chosen as the primary address).
var virtualKeywords = []string{
	"virtualbox", "vmware", "hyper-v", "vethernet", "vswitch", "wsl",
	"docker", "container", "bridge", "tailscale", "zerotier", "hamachi",
	"loopback", "pseudo", "isatap", "teredo", "tap-", "tun", "virtual",
}

func isVirtual(name string) bool {
	l := strings.ToLower(name)
	for _, k := range virtualKeywords {
		if strings.Contains(l, k) {
			return true
		}
	}
	return false
}

// isPrivate reports whether ip is in an RFC1918 range (10/8, 172.16/12,
// 192.168/16) - i.e. a normal LAN address.
func isPrivate(ip net.IP) bool {
	ip4 := ip.To4()
	if ip4 == nil {
		return false
	}
	switch {
	case ip4[0] == 10:
		return true
	case ip4[0] == 192 && ip4[1] == 168:
		return true
	case ip4[0] == 172 && ip4[1] >= 16 && ip4[1] <= 31:
		return true
	}
	return false
}

// rank orders addresses by usefulness for a LAN client: real private LAN
// address first, then any other real address, then virtual NICs, then
// link-local (APIPA), and finally loopback.
func rank(a Address) int {
	switch {
	case a.Loopback:
		return 5
	case a.Private && !a.Virtual:
		return 0
	case a.Private && a.Virtual:
		return 1
	case a.LinkLocal:
		return 4
	case a.Virtual:
		return 3
	default:
		return 2
	}
}

// Enumerate returns every IPv4 address currently assigned to an UP interface,
// best candidate first. Duplicate IPs (aliases on the same NIC) are dropped.
func Enumerate() []Address {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	out := make([]Address, 0, len(ifaces)*2)
	seen := make(map[string]struct{})
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		addrs, aerr := iface.Addrs()
		if aerr != nil {
			continue
		}
		virt := isVirtual(iface.Name)
		for _, a := range addrs {
			ipnet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			ip4 := ipnet.IP.To4()
			if ip4 == nil {
				continue
			}
			addr := Address{
				IP:        ip4.String(),
				Interface: iface.Name,
				Loopback:  ip4.IsLoopback(),
				Private:   isPrivate(ip4),
				LinkLocal: ip4.IsLinkLocalUnicast(),
				Virtual:   virt || ip4.IsLoopback(),
			}
			if _, dup := seen[addr.IP]; dup {
				continue
			}
			seen[addr.IP] = struct{}{}
			out = append(out, addr)
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		ri, rj := rank(out[i]), rank(out[j])
		if ri != rj {
			return ri < rj
		}
		return out[i].Interface < out[j].Interface
	})
	return out
}

// LAN returns the addresses a remote machine on the LAN could plausibly reach:
// everything except loopback.
func LAN() []Address {
	var out []Address
	for _, a := range Enumerate() {
		if a.Loopback {
			continue
		}
		out = append(out, a)
	}
	return out
}

// Primary returns the best single address to advertise, falling back to
// loopback when the machine has no network at all.
func Primary() string {
	if addrs := Enumerate(); len(addrs) > 0 {
		for _, a := range addrs {
			if !a.Loopback {
				return a.IP
			}
		}
		return addrs[0].IP
	}
	return "127.0.0.1"
}

// IsWildcard reports whether host means "listen on / represent every local
// address": empty, the 0.0.0.0 / :: wildcards, or the literal "auto".
func IsWildcard(host string) bool {
	h := strings.TrimSpace(host)
	switch {
	case h == "":
		return true
	case strings.EqualFold(h, "auto"):
		return true
	case h == "0.0.0.0", h == "::", h == "[::]", h == "*":
		return true
	}
	return false
}

// IsLoopbackBind reports whether host restricts listeners to this machine only.
func IsLoopbackBind(host string) bool {
	h := strings.TrimSpace(host)
	return h == "127.0.0.1" || h == "::1" || strings.EqualFold(h, "localhost")
}

// ResolvePublicHost turns the configured server.host into a host usable inside
// URLs handed to clients. Wildcards resolve to the primary LAN address so that
// an RTSP/WebRTC URL shown on another machine is actually dialable.
func ResolvePublicHost(host string) string {
	if !IsWildcard(host) {
		return strings.TrimSpace(host)
	}
	return Primary()
}
