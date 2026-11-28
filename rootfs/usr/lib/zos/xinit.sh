#!/bin/sh
# run by startx: X on /dev/fb0, then ~/.xinitrc, or twm with an xterm.
# leaving the window manager (twm: click the background, Exit) ends X
export DISPLAY=:0
mkdir -p /tmp/.X11-unix
Xorg :0 -nolisten tcp -noreset -logfile /tmp/Xorg.0.log vt1 >/tmp/Xorg.out 2>&1 &
xpid=$!
i=0
while [ $i -lt 40 ] && ! xsetroot -solid steelblue 2>/dev/null; do
    if ! kill -0 $xpid 2>/dev/null; then
        echo "startx: Xorg failed, see /tmp/Xorg.0.log"
        exit 1
    fi
    sleep 0.5
    i=$((i + 1))
done
if [ -f "$HOME/.xinitrc" ]; then
    sh "$HOME/.xinitrc"
else
    xterm -geometry 80x24+40+40 &
    twm
fi
kill $xpid 2>/dev/null
wait $xpid 2>/dev/null
