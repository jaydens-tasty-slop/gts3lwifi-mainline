Round 14: test images only, no new module. Same v9 modules and the round 8
boot image. Normal boot (fastboot boot boot-display-touch.img).

Copy the six .gz files to the tablet, then show each one and take a PHOTO
of the whole screen for each (please photograph, do not describe):
  scp p*.raw.gz test@172.16.42.1:
  ssh test@172.16.42.1
  for f in p1-leftbars p2-rightbars p3-red-blue p4-leftramp p5-leftstripes p6-leftredramp; do
    echo "== $f"; zcat $f.raw.gz | sudo tee /dev/fb0 > /dev/null; sleep 8
  done
(8 seconds per image; the name is printed in the terminal before each one.)

What they are:
  p1  colour bars in the LEFT half of the picture only, right half black
  p2  colour bars in the RIGHT half only, left half black
  p3  left half red, right half blue
  p4  grey ramp (black to white) in the left half, right half black
  p5  thin red/black stripes in the left half, right half black
  p6  red ramp in the left half, right half black

Send the six photos (named or in order).
