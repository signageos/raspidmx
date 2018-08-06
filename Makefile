TARGETS=lib \
	life \
	mandelbrot \
	offscreen \
	pngview \
	pngview_refresh \
	radar_sweep \
	radar_sweep_alpha \
	rgb_triangle \
	game \
	spriteview \
	test_pattern \
	worms

default :all

all:
	for target in $(TARGETS); do ($(MAKE) -C $$target); done

clean:
	for target in $(TARGETS); do ($(MAKE) -C $$target clean); done

