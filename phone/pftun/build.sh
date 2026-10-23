#!/usr/bin/env bash
# Run on your computer (needs git, tar, curl, ssh). Sets up a build environment on the phone and
# builds pftun and pfdump there. The XNU headers and the iOS SDK are downloaded here, not committed.
#   PHONE=<phone ip or host> ./build.sh        optional: KEY=<ssh private key>, PHONE_USER=<user, default root>
set -euo pipefail
PHONE=${PHONE:?set PHONE to the address of the phone}
SRC=$(cd "$(dirname "$0")" && pwd)
KEYOPT=()
[ -n "${KEY:-}" ] && KEYOPT=(-i "$KEY")
SSH=(ssh -o BatchMode=yes "${KEYOPT[@]}" "${PHONE_USER:-root}@$PHONE")
W=$(mktemp -d); cd "$W"
B=https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-10063.141.1
mkdir -p inc/net inc/libkern inc/sys
for f in pfvar.h radix.h pf_pbuf.h if_pflog.h; do curl -sfL $B/bsd/net/$f -o inc/net/$f; done
curl -sfL $B/libkern/libkern/tree.h -o inc/libkern/tree.h
for f in kern_control.h sys_domain.h; do curl -sfL $B/bsd/sys/$f -o inc/sys/$f; done
git clone -q --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks.git sdks
(cd sdks && git sparse-checkout set --no-cone '/iPhoneOS16.5.sdk/usr/include/*' '/iPhoneOS16.5.sdk/usr/lib/libSystem*' '/iPhoneOS16.5.sdk/usr/lib/system/*.tbd')
find sdks/iPhoneOS16.5.sdk/usr/lib -name '*.tbd' -exec sed -i 's/\r$//' {} +
cp sdks/iPhoneOS16.5.sdk/usr/lib/libSystem.B.tbd sdks/iPhoneOS16.5.sdk/usr/lib/libSystem.tbd   # not a symlink
"${SSH[@]}" 'export PATH=/var/jb/usr/bin:$PATH; mkdir -p /var/mobile/pfb; command -v clang >/dev/null || for i in 1 2 3 4; do apt-get install -y -o Acquire::Retries=5 --fix-missing network-cmds tcpdump clang ldid && break; done'
tar cf - -C sdks/iPhoneOS16.5.sdk usr | "${SSH[@]}" 'mkdir -p /var/mobile/pfb/sdk && tar xf - -C /var/mobile/pfb/sdk'
tar cf - inc -C "$SRC" pftun.c pfdump.c | "${SSH[@]}" 'tar xf - -C /var/mobile/pfb'
"${SSH[@]}" 'export PATH=/var/jb/usr/bin:$PATH; cd /var/mobile/pfb; for p in pftun pfdump; do clang -arch arm64 -isysroot sdk -Iinc -DPRIVATE -D__APPLE_API_PRIVATE -miphoneos-version-min=15.0 -Wno-everything $p.c -o $p && ldid -S $p; done; ls -l pftun pfdump'
echo "built: /var/mobile/pfb/pftun on the phone (phone/install.sh --pftun picks it up from there)"
