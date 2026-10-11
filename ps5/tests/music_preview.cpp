// The shelf's music tracks on the PC: music_preview out-prefix [seconds] -> <prefix>-NN-<name>.wav (fe_music.cpp)
#include "../frontend/fe_music.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	const int seconds = argc > 2 ? atoi(argv[2]) : 40;
	const int frames = 48000 * seconds;
	for (int t = 0; t < fe::music::TrackCount(); t++)
	{
		std::vector<int16_t> pcm(size_t(frames) * 2);
		fe::music::RenderTrack(t, pcm.data(), frames, 0.6f); // (the shelf mixes the music at 0.6)
		std::string name = fe::music::TrackName(t);
		for (char& c : name)
			if (c == ' ')
				c = '_';
		char path[512];
		snprintf(path, sizeof(path), "%s-%02d-%s.wav", argv[1], t + 1, name.c_str());
		FILE* f = fopen(path, "wb");
		const unsigned data = unsigned(pcm.size() * 2), riff = 36 + data, fmt = 16, rate = 48000, bps = 48000 * 4;
		const unsigned short pcm1 = 1, ch = 2, align = 4, bits = 16;
		fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
		fwrite(&pcm1, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f);
		fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
		fwrite(pcm.data(), 2, pcm.size(), f);
		fclose(f);
		double sum = 0;
		int peak = 0, clip = 0;
		for (int16_t v : pcm)
			sum += double(v) * v, peak = std::max(peak, std::abs(int(v))), clip += std::abs(int(v)) >= 32700;
		printf("%-18s rms %.3f peak %.2f clipped %d -> %s\n", fe::music::TrackName(t), std::sqrt(sum / pcm.size()) / 32767,
			peak / 32767.0, clip, path);
	}
}
