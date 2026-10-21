#!/bin/sh
# Render long monolith drones on the PC (sanitizers on). Usage from this folder:
#   ./render_demo.sh <name> <seconds> <knobs start a,b,c,d> <knobs end a,b,c,d> <midi notes n,n,...>
#   ./render_demo.sh dark 90 7000,14000,4000,20000 7000,14000,4000,20000 24,36
set -e
mkdir -p build out
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -DOP1_HOST -I ../.. drone_demo.c monolith.c -lm -o build/drone_demo
./build/drone_demo "$2" "$3" "$4" "$5" > "out/$1.raw"
python3 -c "import wave,sys; d=open(sys.argv[1],'rb').read(); w=wave.open(sys.argv[2],'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(d)" "out/$1.raw" "out/$1.wav"
rm "out/$1.raw"; echo "-> out/$1.wav"
