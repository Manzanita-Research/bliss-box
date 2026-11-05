# Somebody at the keys

Scripts for `build/render play SCRIPT out.wav mood=N`: a minute or so of playing each mood the way the keys are
meant to be played. Chords held in the left hand, a line over them, a note held alone to move the harmony, a pause
for the melody's answer, then letting go. The notes are the mood's scale degrees (1 the tonic between F3 and E4), so
a script plays in any mood's key; see tools/render.c for the format.

    for m in dawn temple redwoods stars still river; do
      build/render play tools/play/$m.txt renders/audio/play/$m.wav mood=$(grep -m1 mood= tools/play/$m.txt | sed 's/.*mood=//')
    done
