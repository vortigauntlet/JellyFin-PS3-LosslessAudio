# Builds the synthetic fragmented-MP4 AAC fixtures (own tones, so no copyright) shaped like SoundCloud's HLS
# audio: init.mp4 plus ~10 s media segments with styp/sidx/moof/mdat.  Needs ffmpeg.
#   stereo44/   stereo 44.1 kHz AAC-LC 160 kbit/s, 25 s  (3 segments)
#   mono48/     mono 48 kHz AAC-LC 96 kbit/s, 12 s       (2 segments)
# Each directory also gets, from ffmpeg's own demuxer and decoder run over the same bytes:
#   frames.adts  the AAC frames in order (-c copy -f adts), the reference for the demux test
#   ref.f32      the decode as interleaved float32 little-endian, the reference for the decode test
cd "$(dirname "$0")" || exit 1
set -e
mk() {
  name=$1; rate=$2; ch=$3; br=$4; dur=$5
  rm -rf "$name"; mkdir "$name"
  if [ "$ch" = 2 ]; then
    ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=$dur:sample_rate=$rate" -f lavfi -i "sine=frequency=660:duration=$dur:sample_rate=$rate" \
      -filter_complex "[0][1]amerge=inputs=2,volume=0.5" -c:a pcm_s16le -f wav "$name/src.wav"
  else
    ffmpeg -v error -y -f lavfi -i "sine=frequency=523:duration=$dur:sample_rate=$rate" -af "volume=0.5" -ac 1 -c:a pcm_s16le -f wav "$name/src.wav"
  fi
  ffmpeg -v error -y -i "$name/src.wav" -c:a aac -b:a "$br" -f dash -seg_duration 10 -use_template 1 -use_timeline 1 \
    -init_seg_name init.mp4 -media_seg_name 'data$Number%03d$.m4s' -adaptation_sets "id=0,streams=a" "$name/manifest.mpd"
  (cd "$name" && cat init.mp4 data*.m4s > all.mp4)
  ffmpeg -v error -y -i "$name/all.mp4" -c:a copy -f adts "$name/frames.adts"
  ffmpeg -v error -y -i "$name/all.mp4" -f f32le -acodec pcm_f32le "$name/ref.f32"
  rm -f "$name/src.wav" "$name/manifest.mpd" "$name/all.mp4"
}
mk stereo44 44100 2 160k 25
mk mono48 48000 1 96k 12
ls -la stereo44 mono48
