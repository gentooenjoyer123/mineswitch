# Horizon IPv4 native networking

The first libnet port keeps the shared OpenJDK JNI code and uses libnx's BSD
socket functions for IPv4 sockets, socket options, `poll`, DNS (`getaddrinfo`
and `getnameinfo`) and hostname queries. `InetAddress.isReachable` uses the
existing TCP echo-port fallback; the SDK's absent raw ICMP headers are not
replaced with invented packet structures.

`NetworkInterface` reads the kernel interface list with `SIOCGIFCONF`, handles
BSD variable-length entries, and queries real interface indexes, flags and MTU
with the corresponding ioctls. Hardware addresses are returned only when a
matching `AF_LINK` record is present in the kernel list. The pinned libnx BSD
adapter explicitly marshals the nested `SIOCGIFCONF` buffer.

The current backend reports IPv6 unavailable. The installed libnx header omits
the traffic-class and IPv6 membership API needed by the generic OpenJDK code.
Invoking the unused Inet6 native implementation raises an explicit unsupported
operation; IPv4 is selected through OpenJDK's normal capability detection.
OS proxy discovery likewise reports that provider unavailable, while Java's
explicit proxy properties and `ProxySelector` remain available.

The SocketOptionRegistry generator uses actual target headers. Its flags now
participate in incremental dependencies, preventing stale platform constants
after a configure change. `IPV6_TCLASS` is omitted when the target SDK does not
define it, instead of borrowing an option number from Linux.

`libnet.a` compiles and its undefined symbols are supplied by the other target
libraries and SDK. These are build checks. DNS, TCP/UDP, interface enumeration
and Java selector behavior still require execution on the device before the
networking port can be called operational.
