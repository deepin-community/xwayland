# Reporting Security Issues

Please notify us of any security issues by sending mail to
<xorg-security@lists.x.org>.

See https://www.x.org/wiki/Development/Security/Organization/
for more information about the X.Org security team.

# Learning about Security Fixes

X.Org announces security bugs and bug fix releases on the xorg-announce
mailing list.  See the archives at https://lists.x.org/archives/xorg-announce/
and see https://lists.x.org/mailman/listinfo/xorg-announce to subscribe.

Security advisories are also listed on our wiki at
https://www.x.org/wiki/Development/Security/ and mailed to the
https://oss-security.openwall.org/wiki/mailing-lists/oss-security mailing list.

# Security model and trust boundaries

Xwayland is expected to run with only the privileges of the user who started
the server.  It should not require direct access to any devices.

Access control for which clients can connect to the X server is provided by
a number of mechanisms, see the Xsecurity(7) man page for details.  Once a
client is authenticated via those mechanisms and has an active connection,
we do not consider it a security vulnerability for them to be able to take
any actions described in the X11 protocol or extension specifications, such
as changing monitor configurations or killing other clients, though we will
accept non-security bug reports for clients doing so in a manner or via
requests not documented in the protocol specs as doing those operations.
