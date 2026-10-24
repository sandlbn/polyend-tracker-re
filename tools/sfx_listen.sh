#!/bin/sh
# Blind listening test for the Sample Editor speed work (tests/sfx_render.cpp):
# per case it plays the original, then A, then B, and asks which one is the new
# code (or that they sound the same). The answers are checked against key.txt
# at the end, so nothing is revealed while you listen.
#
#   make -C tests sfx-render                 # writes build/bench/sfx_listen
#   tools/sfx_listen.sh [FILTER]             # e.g. tools/sfx_listen.sh loop__eq
#   GAP=2 tools/sfx_listen.sh                # seconds of silence between clips (1.5)
#   MIN=5 tools/sfx_listen.sh                # short clips repeat to last MIN s (3)
#   STEP=1 tools/sfx_listen.sh               # wait for Enter before each clip
#   ABX=1 tools/sfx_listen.sh [FILTER]       # ABX: A, B, then X (A or B at random);
#                                            # say which X is, TRIALS times (8) per case
#   DIFF=1 tools/sfx_listen.sh [FILTER]      # just play the differences new - old:
#                                            # real level, then amplified x64 (+36 dB)
#
# Keys per case: a / b = that one is the new code, s = same / can't tell,
# r = replay, o = original again, n = skip, q = quit (the score so far).
set -u
dir=$(cd "$(dirname "$0")/.." && pwd)/build/bench/sfx_listen
filter=${1:-}
gap=${GAP:-1.5}
min=${MIN:-3}
step=${STEP:-0}
abx=${ABX:-0}
trials=${TRIALS:-8}
diffmode=${DIFF:-0}
[ -f "$dir/key.txt" ] || { echo "sfx_listen: no $dir/key.txt; run make -C tests sfx-render first" >&2; exit 2; }
for p in pw-play paplay aplay; do
    command -v "$p" >/dev/null 2>&1 && { player=$p; break; }
done
[ -n "${player:-}" ] || { echo "sfx_listen: no audio player (pw-play, paplay or aplay)" >&2; exit 2; }

# Seconds in a 16-bit 44.1 kHz WAV (the header is 44 bytes; mono or stereo).
seconds() {
    bytes=$(wc -c < "$1")
    ch=$(od -An -tu2 -j22 -N2 "$1" | tr -d ' ')
    awk -v b="$bytes" -v c="$ch" 'BEGIN { printf "%.2f", (b - 44) / (2 * c * 44100) }'
}

play() {   # LABEL FILE
    if [ ! -s "$2" ]; then
        echo "  $1: missing $2 (run make -C tests sfx-render)"
        return
    fi
    len=$(seconds "$2")
    reps=$(awk -v l="$len" -v m="$min" 'BEGIN { r = (l > 0 && l < m) ? int(m / l + 0.999) : 1; print r }')
    if [ "$step" = 1 ]; then
        printf '  %-9s (%ss%s) press Enter to play' "$1" "$len" "$([ "$reps" -gt 1 ] && echo " x$reps")"
        read -r _ </dev/tty
    else
        printf '  %-9s (%ss%s) ' "$1" "$len" "$([ "$reps" -gt 1 ] && echo " x$reps")"
    fi
    n=0
    while [ $n -lt "$reps" ]; do
        "$player" "$2" >/dev/null 2>&1
        n=$((n + 1))
    done
    printf 'done\n'
    sleep "$gap"
}

# Chance of getting at least K of N right by guessing (one-sided binomial, p = 1/2).
chance() {
    awk -v k="$1" -v n="$2" 'BEGIN { s = 0; for (i = k; i <= n; i++) { c = 1; for (j = 1; j <= i; j++) c = c * (n - i + j) / j; s += c } printf "%.3f", s / 2 ^ n }'
}
coin() { [ $(( $(od -An -tu1 -N1 /dev/urandom) % 2 )) -eq 0 ]; }

if [ "$diffmode" = 1 ]; then
    # Case lines only ("name  max N LSB ..."), not the header or identical cases.
    grep -E '^[^ ]+__[^ ]+ +max ' "$dir/levels.txt" | grep -e "$filter" | while read -r c rest; do
        echo
        echo "$c: $rest"
        play "diff" "$dir/${c}__diff.wav"
        play "diff x64" "$dir/${c}__diff_x64.wav"
    done
    exit 0
fi

if [ "$abx" = 1 ]; then
    cases=$(sed -n 's/^\([^ ]*__[^ ]*\) .*new = .*/\1/p' "$dir/key.txt" | grep -e "$filter")
    summary=$(mktemp)
    trap 'rm -f "$summary"' EXIT
    for c in $cases; do
        echo
        echo "ABX: $c  ($trials trials)"
        ok=0; t=0
        while [ $t -lt "$trials" ]; do
            t=$((t + 1))
            if coin; then x=A; else x=B; fi
            echo " trial $t/$trials"
            play A "$dir/${c}__A.wav"
            play B "$dir/${c}__B.wav"
            play X "$dir/${c}__${x}.wav"
            while :; do
                printf '  X is: a / b   r(eplay A B X) x(replay X) q(uit): '
                read -r k </dev/tty || k=q
                case $k in
                    a|A|b|B) g=$(echo "$k" | tr 'ab' 'AB'); [ "$g" = "$x" ] && ok=$((ok + 1)); break ;;
                    r) play A "$dir/${c}__A.wav"; play B "$dir/${c}__B.wav"; play X "$dir/${c}__${x}.wav" ;;
                    x) play X "$dir/${c}__${x}.wav" ;;
                    q) t=$((t - 1)); break 3 ;;
                esac
            done
        done
        p=$(chance "$ok" "$t")
        echo "  $c: $ok / $t right  (chance by guessing: $p)" | tee -a "$summary"
    done
    echo
    echo "ABX results (5 of 10 or so = no audible difference; a small 'chance' like < 0.05 = you heard it):"
    cat "$summary" 2>/dev/null
    exit 0
fi

answers=$(mktemp)
trap 'rm -f "$answers"' EXIT
# The cases worth the most attention first: the low-frequency EQ ones.
cases=$(sed -n 's/^\([^ ]*__[^ ]*\) .*new = .*/\1/p' "$dir/key.txt" |
        awk '{ p = ($0 ~ /eq-10hz|bass-boost/) ? 0 : ($0 ~ /__eq/ ? 1 : 2); print p, $0 }' | sort -k1,1n -k2 | cut -d' ' -f2 |
        grep -e "$filter")
total=$(echo "$cases" | grep -c .)
i=0
for c in $cases; do
    i=$((i + 1))
    sig=${c%%__*}
    echo
    echo "[$i/$total] $c"
    play original "$dir/${sig}__original.wav"
    play A "$dir/${c}__A.wav"
    play B "$dir/${c}__B.wav"
    while :; do
        printf '  which is the new code? a / b / s(ame)   r(eplay A B) o(riginal) n(ext) q(uit): '
        read -r k </dev/tty || k=q
        case $k in
            a|A|b|B|s|S) echo "$c $(echo "$k" | tr 'ab' 'AB' | tr 's' 'S')" >> "$answers"; break ;;
            r) play A "$dir/${c}__A.wav"; play B "$dir/${c}__B.wav" ;;
            o) play original "$dir/${sig}__original.wav" ;;
            n) break ;;
            q) break 2 ;;
        esac
    done
done

echo
echo "Results (new = the float code):"
right=0; wrong=0; same=0
while read -r c a; do
    truth=$(sed -n "s/^$c  *new = \([AB]\).*(max difference \([0-9]*\) LSB).*/\1 \2/p" "$dir/key.txt")
    t=${truth% *}; lsb=${truth#* }
    case $a in
        S) same=$((same + 1)); verdict="same / can't tell" ;;
        "$t") right=$((right + 1)); verdict="picked the new one" ;;
        *) wrong=$((wrong + 1)); verdict="picked the old one" ;;
    esac
    printf '  %-40s you: %s  new: %s  (%s LSB)  %s\n' "$c" "$a" "$t" "$lsb" "$verdict"
done < "$answers"
echo "  picked the new one: $right   picked the old one: $wrong   same / can't tell: $same"
echo "  (if you can't hear a difference, right and wrong come out about even, or mostly 'same')"
