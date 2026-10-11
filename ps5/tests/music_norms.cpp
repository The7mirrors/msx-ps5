// Prints fe_music's kNormTable (each track's loudness scale), measured on the PC.
#include "../frontend/fe_music.h"
#include <cstdio>
int main()
{
	printf("const float kNormTable[] = {");
	for (int t = 0; t < fe::music::TrackCount(); t++)
		printf("%s%.4ff", t ? ", " : "", fe::music::MeasureNorm(t));
	printf("};\n");
}
