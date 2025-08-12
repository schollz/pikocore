build16: pico-sdk changeto16 quick

build4: pico-sdk changeto4 quick

build2: pico-sdk changeto2 quick

prereqs: pico-sdk
ifeq ($(shell uname),Darwin)
	brew install clang-format cmake gcc minicom sox python3
	brew install --cask gcc-arm-embedded
	python3 -m pip install --break-system-packages matplotlib numpy icecream
else
	sudo apt install -y clang-format cmake gcc-arm-none-eabi gcc g++ minicom sox python3 python3-pip
	sudo -H python3 -m pip install --break-system-packages matplotlib numpy icecream
endif

doth/easing.h:
	cd doth && python3 generate_easing.py > easing.h
	clang-format -i --style=google doth/easing.h

doth/filter.h:
	cd doth && python3 biquad.py > filter.h
	clang-format -i --style=google doth/filter.h

quick: doth/easing.h doth/filter.h
	mkdir -p build
	cd build && cmake ..
	cd build && make -j4
	echo "BUILD SUCCESS"

changeto16:
	sed -i 's/(2 \* 1024 \* 1024)/(16 \* 1024 \* 1024)/g' CMakeLists.txt
	sed -i 's/(4 \* 1024 \* 1024)/(16 \* 1024 \* 1024)/g' CMakeLists.txt

changeto2:
	sed -i 's/(16 \* 1024 \* 1024)/(2 \* 1024 \* 1024)/g' CMakeLists.txt
	sed -i 's/(4 \* 1024 \* 1024)/(2 \* 1024 \* 1024)/g' CMakeLists.txt

changeto4:
	sed -i 's/(16 \* 1024 \* 1024)/(4 \* 1024 \* 1024)/g' CMakeLists.txt
	sed -i 's/(2 \* 1024 \* 1024)/(4 \* 1024 \* 1024)/g' CMakeLists.txt

audio:
	cd audio2h && rm -rf converted
	cd audio2h && mkdir converted
	cd audio2h && go run main.go --limit 1 --bpm 165 --sr 31000 --folder-in demo

clean:
	rm -rf build
	rm -rf doth/easing.h
	rm -rf doth/filter.h
	rm -rf doth/audio2h.h
	rm -rf audio2h/converted
	rm -rf audio2h/files.json

pico-sdk:
	git clone https://github.com/raspberrypi/pico-sdk
	cd pico-sdk && git checkout 2.1.1
	cd pico-sdk && git submodule update --init

debug:
	sudo minicom -b 115200 -o -D /dev/ttyACM0
