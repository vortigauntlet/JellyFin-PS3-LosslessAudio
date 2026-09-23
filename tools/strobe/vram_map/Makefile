# Builds vram_map against YOUR toolchain's allocator source, unmodified:
#   make -C tools/strobe/vram_map PSL1GHT=$PSL1GHT
#   ./tools/strobe/vram_map/vram_map tools/strobe/vram_map/tip_1080p.example.txt
vram_map: vram_map.c
	@test -n "$(PSL1GHT)" || { echo "set PSL1GHT to the PSL1GHT source tree"; exit 1; }
	$(CC) -O2 -Wall -Ihoststub -I$(PSL1GHT)/ppu/include/sys -o $@ vram_map.c $(PSL1GHT)/ppu/librt/heap.c
clean:
	rm -f vram_map
