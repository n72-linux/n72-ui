#!/bin/sh
# train.sh: PGO training of the instrumented n72-ui (/root/n72-ui-pgogen) on the
# iPod, panel lit, through the fake-touch harness (tseq2-remote.sh); each run
# writes /root/pgo/ui-<pid>.profraw when it is stopped (SIGTERM).
mkdir -p /root/pgo; rm -f /root/pgo/*.profraw
PANEL=/sys/devices/platform/soc/38900000.lcd/panel_power
V=/tmp/tscript-v; H=/tmp/tscript-h
# vertical: three slow drags up, a fling, a drag back down (the Files drags)
cp /tmp/tscript $V
# horizontal: home page swipes, slow and flung, both ways
{
	echo "s 4000"
	for rep in 1 2; do
		echo "d 280 240"; x=280; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do x=$((x-20)); echo "s 16"; echo "m $x 240"; done; echo "u"; echo "s 1200"
		echo "d 40 240"; x=40; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do x=$((x+20)); echo "s 16"; echo "m $x 240"; done; echo "u"; echo "s 1200"
		echo "d 260 240"; x=260; for i in 1 2 3 4; do x=$((x-50)); echo "s 16"; echo "m $x 240"; done; echo "u"; echo "s 1200"
		echo "d 60 240"; x=60; for i in 1 2 3 4; do x=$((x+50)); echo "s 16"; echo "m $x 240"; done; echo "u"; echo "s 1200"
	done
} > $H
# keyboard: taps across the letter rows
{
	echo "s 4000"
	for rep in 1 2 3; do
		for x in 16 48 80 112 144 176 208 240 272 304; do echo "d $x 330"; echo "s 60"; echo "u"; echo "s 120"; done
		for x in 32 64 96 128 160 192 224 256 288; do echo "d $x 375"; echo "s 60"; echo "u"; echo "s 120"; done
	done
} > /tmp/tscript-k
echo "XENV=LLVM_PROFILE_FILE=/root/pgo/ui-%p.profraw" > /tmp/xenv
run() {	# page script seconds
	cp /root/n72-ui-pgogen /tmp/n72-ui-test
	cp /root/tools/n72touch /tmp/n72touch
	cp $2 /tmp/tscript
	sh /tmp/tseq2-remote.sh "$1" >/dev/null 2>&1 &
	sleep $3
	killall n72-ui-test n72touch 2>/dev/null; [ -f /tmp/tf.pid ] && kill $(cat /tmp/tf.pid) 2>/dev/null
	sleep 1
	echo "$1: $(ls /root/pgo | wc -l) profiles"
}
run home $H 22
run files:/usr/lib $V 16
run files:/root $V 16
run logs $V 16
run sound $V 14
run display $V 14
run wifi $V 14
run keyboard /tmp/tscript-k 16
cp $V /tmp/tscript
# the redraw benchmark (text, labels, panels): exits by itself
( cd /tmp; LLVM_PROFILE_FILE=/root/pgo/bench-%p.profraw /root/n72-ui-pgogen --bench >/root/pgo/bench.log 2>&1 )
rm -f /tmp/n72-ui-test /tmp/n72touch /tmp/xenv /tmp/tscript-v /tmp/tscript-h /tmp/tscript-k
ls -l /root/pgo
rc-service n72-ui start >/dev/null 2>&1	# (the harness stopped it)
sleep 3; echo 1 > $PANEL
