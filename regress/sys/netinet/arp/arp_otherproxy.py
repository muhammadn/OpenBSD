#!/usr/local/bin/python2.7
# send Address Resolution Protocol Request for Proxy ARP on other interface
# expect no answer

import os
from addr import *
from scapy.all import *

arp=ARP(op='who-has', hwsrc=LOCAL_MAC, psrc=LOCAL_ADDR,
    hwdst="ff:ff:ff:ff:ff:ff", pdst=OTHERFAKE_ADDR)
eth=Ether(src=LOCAL_MAC, dst="ff:ff:ff:ff:ff:ff")/arp

e=srp1(eth, iface=LOCAL_IF, timeout=2)

if e and e.type == ETH_P_ARP:
	a=e.payload
	a.show()
	print "ARP REPLY"
	exit(1)

print "no arp reply"
exit(0)