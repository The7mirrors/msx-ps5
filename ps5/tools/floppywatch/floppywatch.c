/* floppywatch (2026-10-22, AI-assisted; user request: "I want it to boot automatically if the drive is connected to the PS5").
 *
 * Watches the PS5's USB disks (/dev/da0..15) for a floppy disk (160 KB to 2.88 MB). When one appears that holds an MSX game
 * (a PC floppy with a .rom / .mx1 / .mx2 / .dsk / .cas / .zip on it, or an MSX disk of its own), it starts MSX PS5
 * (PPSA99231), which then starts that game by itself (MSX PS5 1.2: a floppy's game starts without the shelf). Once per disk:
 * the same disk left in does nothing more; take it out and put it back (or plug the drive in again) and it starts again.
 * MSX PS5 already open: nothing to do (it sees the disk itself). Another of the user's apps or games open: no launch over it,
 * a notification instead. Reading only: nothing is written to the floppy. Log: /data/msx/logs/floppywatch.log.
 * Built with the ps5-payload-dev SDK, with MSX PS5's own floppy reader (fe_floppy.c). SPDX-License-Identifier: MIT */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fe_floppy.h" // (ps5/frontend)

#define VERSION "1.0"
#define MSX_TITLE "PPSA99231"
#define LOG_DIR "/data/msx/logs"
#define LOCK_PORT 9133
#define POLL_MS 2000

typedef struct
{
	uint32_t sz;
	uint32_t user_id;
	uint32_t app_opt;
	uint64_t crash_report;
	uint32_t check_flag;
} LncAppParam;
int sceLncUtilLaunchApp(const char* title_id, const char* argv[], LncAppParam* param);
int sceLncUtilGetAppId(const char* title_id);
int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int32_t* user);
int sceUserServiceGetInitialUser(int32_t* user);

/* the PS5's notification request (0xC30 bytes), as discwatch sends it */
typedef struct
{
	int32_t type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
	char use_icon_image_uri;
	char message[1024];
	char uri[1024];
	char unkstr[1024];
	char pad[3];
} notify_request_t;
_Static_assert(sizeof(notify_request_t) == 0xC30, "notification request must be 0xC30 bytes");
int sceKernelSendNotificationRequest(int32_t device, void* request, size_t size, int32_t blocking);

/* Homebrew apps and emulators often found on a PS5 (title IDs): when one of them is open, MSX PS5 isn't started over it. */
static const char* const kOthers[] = {"PPSA99213", "PPSA99203", "PPSA50011", "PPSA97358", "PPSA99360", "PPSA99008", "PPSA99250",
	"PPSA99232", "PPSA99233", "PPSA99241", "PPSA99764", "PPSA99248", "PPSA99012", "PPSA99221", "PPSA99222", "PPSA99223", "PPSA99224"};

static FILE* g_log;
static void log_line(const char* fmt, ...)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(stamp, sizeof stamp, "%m-%d %H:%M:%S", &tm);
	char msg[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	printf("[floppywatch] %s\n", msg);
	if (g_log)
	{
		fprintf(g_log, "%s %s\n", stamp, msg);
		fflush(g_log);
	}
}

static void notify(const char* msg)
{
	static notify_request_t req;
	memset(&req, 0, sizeof req);
	req.target_id = -1;
	snprintf(req.message, sizeof req.message, "%s", msg);
	sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
	log_line("notify: %s", msg);
}

static int has_ext(const char* name, const char* ext)
{
	const size_t n = strlen(name), e = strlen(ext);
	return n > e && strcasecmp(name + n - e, ext) == 0;
}

/* The disk's first game file ("" and 1 for an MSX disk of its own; 0 when it holds nothing to play). */
static int disk_game(const char* dev, long long size, char* game, size_t cap)
{
	static fe_floppy_file files[128];
	game[0] = '\0';
	const int n = fe_floppy_files(dev, files, 128);
	static const char* const kExts[] = {".rom", ".mx1", ".mx2", ".dsk", ".cas", ".zip"};
	for (int i = 0; i < n; i++)
		for (size_t e = 0; e < sizeof kExts / sizeof kExts[0]; e++)
			if (has_ext(files[i].name, kExts[e]))
			{
				snprintf(game, cap, "%s", files[i].name);
				return 1;
			}
	if (n == 0 && size < 1400000)
		return 1; /* a 720 / 360 KB disk that isn't a PC one: an MSX disk */
	return 0;
}

static const char* other_app_open(void)
{
	for (size_t i = 0; i < sizeof kOthers / sizeof kOthers[0]; i++)
		if (sceLncUtilGetAppId(kOthers[i]) > 0)
			return kOthers[i];
	return NULL;
}

static void launch_msx(const char* what)
{
	int32_t user = -1;
	if (sceUserServiceGetForegroundUser(&user) != 0 || user < 0)
		sceUserServiceGetInitialUser(&user);
	LncAppParam p;
	memset(&p, 0, sizeof p);
	p.sz = sizeof p;
	p.user_id = (uint32_t)user;
	int rc = -1;
	for (int attempt = 0; attempt < 3; attempt++)
	{
		rc = sceLncUtilLaunchApp(MSX_TITLE, NULL, &p);
		log_line("launch MSX PS5 for %s: attempt %d, user %d -> 0x%x", what, attempt + 1, user, (unsigned)rc);
		if (rc >= 0)
			return;
		usleep(700000);
	}
	notify("MSX floppy: MSX PS5 couldn't be started. Open it from the home screen.");
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	/* one watcher only: the second finds the port taken and leaves */
	const int lock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(LOCK_PORT);
	a.sin_addr.s_addr = htonl(0x7F000001);
	if (lock < 0 || bind(lock, (struct sockaddr*)&a, sizeof a) != 0)
	{
		printf("[floppywatch] already running\n");
		return 0;
	}
	listen(lock, 1);
	mkdir("/data/msx", 0777);
	mkdir(LOG_DIR, 0777);
	g_log = fopen(LOG_DIR "/floppywatch.log", "a");
	sceUserServiceInitialize(NULL);
	log_line("floppywatch " VERSION " started");

	/* what was in the drives last time, so a disk only starts MSX PS5 when it arrives (one already in at start counts
	   as arriving: the console was just switched on with it plugged in) */
	char seen[FE_FLOPPY_MAX][48];
	int nseen = 0;
	for (;;)
	{
		char devs[FE_FLOPPY_MAX][32];
		long long sizes[FE_FLOPPY_MAX];
		const int n = fe_floppy_list(devs, sizes, FE_FLOPPY_MAX);
		char now[FE_FLOPPY_MAX][48];
		for (int i = 0; i < n; i++)
		{
			snprintf(now[i], sizeof now[i], "%s:%lld", devs[i], sizes[i]);
			int known = 0;
			for (int j = 0; j < nseen; j++)
				known = known || strcmp(seen[j], now[i]) == 0;
			if (known)
				continue;
			char game[200];
			const int playable = disk_game(devs[i], sizes[i], game, sizeof game);
			log_line("%s: a %lld-byte floppy, %s", devs[i], sizes[i], playable ? (game[0] ? game : "an MSX disk") : "no MSX game on it");
			if (!playable)
				continue;
			if (sceLncUtilGetAppId(MSX_TITLE) > 0)
			{
				log_line("MSX PS5 is open: it starts the game itself");
				continue;
			}
			const char* other = other_app_open();
			if (other)
			{
				char msg[300];
				snprintf(msg, sizeof msg, "MSX floppy: %s\nClose the open app, then open MSX PS5 to play it.", game[0] ? game : "MSX disk");
				log_line("%s is open: not starting MSX PS5 over it", other);
				notify(msg);
				continue;
			}
			launch_msx(game[0] ? game : "an MSX disk");
		}
		for (int i = 0; i < n; i++)
			snprintf(seen[i], sizeof seen[i], "%s", now[i]);
		if (n < nseen)
			log_line("a floppy went out (%d left)", n);
		nseen = n;
		usleep(POLL_MS * 1000);
	}
}
