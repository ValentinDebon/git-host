/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>

#include "config.h"

struct git_host_args {
	const char *command;
};

enum git_host_mode {
	GIT_HOST_MODE_NA,
	GIT_HOST_MODE_RO,
	GIT_HOST_MODE_WR,
	GIT_HOST_MODE_RW,
};

static char *
xstrdup(const char *s) {
	char * const c = strdup(s);

	if (c == NULL) {
		err(EXIT_FAILURE, "strdup %s", c);
	}

	return c;
}

static void
git_host_array_push(char *string, int *countp, char ***arrayp) {
	char **array = *arrayp;
	int count = *countp;

	array = realloc(array, sizeof (*array) * ++count);
	if (array == NULL) {
		err(EXIT_FAILURE, "realloc");
	}
	array[count - 1] = string;

	*countp = count;
	*arrayp = array;
}

static void
git_host_expand_command(const char *command, int *argcp, char ***argvp) {
	enum {
		EXPAND_SPACES,
		EXPAND_LITERAL,
		EXPAND_QUOTE_SINGLE,
		EXPAND_QUOTE_DOUBLE,
		EXPAND_QUOTE_DOUBLE_ESCAPE,
		EXPAND_END,
		EXPAND_ERROR_UNCLOSED_QUOTE,
	} state = EXPAND_SPACES;
	char copy[strlen(command) + 1];
	char *it = memcpy(copy, command, sizeof (copy)); /* Local copy for in-place expansion */
	char *dst, *src, *arg;

	*argcp = 0;
	*argvp = NULL;

	while (state < EXPAND_END) {
		switch (state) {
		case EXPAND_SPACES:
			switch (*it) {
			case '\0': state = EXPAND_END; break;
			case ' ':  break;
			case '"':  state = EXPAND_QUOTE_DOUBLE; dst = src = arg = it; src++; break;
			case '\'': state = EXPAND_QUOTE_SINGLE; dst = src = arg = it; src++; break;
			default:   state = EXPAND_LITERAL; dst = src = arg = it; dst++; src++; break;
			}
			break;
		case EXPAND_LITERAL:
			switch (*it) {
			case '\0': state = EXPAND_END; *dst = '\0'; git_host_array_push(xstrdup(arg), argcp, argvp); break;
			case '"':  state = EXPAND_QUOTE_DOUBLE; src++; break;
			case '\'': state = EXPAND_QUOTE_SINGLE; src++; break;
			case ' ':  state = EXPAND_SPACES; *dst = '\0'; git_host_array_push(xstrdup(arg), argcp, argvp); break;
			default:   *dst++ = *src++; break;
			}
			break;
		case EXPAND_QUOTE_SINGLE:
			switch (*it) {
			case '\0': state = EXPAND_ERROR_UNCLOSED_QUOTE; break;
			case '\'': state = EXPAND_LITERAL; src++; break;
			default:   *dst++ = *src++; break;
			}
			break;
		case EXPAND_QUOTE_DOUBLE:
			switch (*it) {
			case '\0': state = EXPAND_ERROR_UNCLOSED_QUOTE; break;
			case '"':  state = EXPAND_LITERAL; src++; break;
			case '\\': state = EXPAND_QUOTE_DOUBLE_ESCAPE; src++; break;
			default:   *dst++ = *src++; break;
			}
			break;
		case EXPAND_QUOTE_DOUBLE_ESCAPE:
			switch (*it) {
			case '\0': state = EXPAND_ERROR_UNCLOSED_QUOTE; break;
			default:   state = EXPAND_QUOTE_DOUBLE; *dst++ = *src++; break;
			}
			break;
		default:
			abort();
		}
		it++;
	}

	if (state == EXPAND_ERROR_UNCLOSED_QUOTE) {
		errx(EXIT_FAILURE, "Invalid command: Unclosed quote");
	}

	if (*argcp == 0) {
		errx(EXIT_FAILURE, "Invalid command: empty command");
	}

	git_host_array_push(NULL, argcp, argvp);
	--*argcp;
}

static char *
git_host_pathcat(const char *dir, const char *sub) {
	const size_t dirlen = strlen(dir), sublen = strlen(sub);
	char path[dirlen + sublen + 2];

	memcpy(path, dir, dirlen);
	path[dirlen] = '/';
	memcpy(path + dirlen + 1, sub, sublen + 1);

	return xstrdup(path);
}

static char *
git_host_execpath(const char *file) {
	const char *execpath = getenv("GIT_EXEC_PATH");

	if (execpath == NULL) {
		execpath = CONFIG_GIT_EXEC_PATH;
	}

	return git_host_pathcat(execpath, file);
}

static int
git_host_normalize_path(char *path) {
	enum {
		NORMALIZE_STATE_TRAILING_SLASH,
		NORMALIZE_STATE_NAME_DOT_DOT,
		NORMALIZE_STATE_NAME_DOT,
		NORMALIZE_STATE_NAME,
	} state = NORMALIZE_STATE_TRAILING_SLASH;
	unsigned int dst = 0, src = 0;

	while (path[src] != '\0') {
		switch (state) {
		case NORMALIZE_STATE_TRAILING_SLASH:
			switch (path[src]) {
			case '/':
				break;
			case '.':
				state = NORMALIZE_STATE_NAME_DOT;
				break;
			default:
				path[dst++] = path[src];
				state = NORMALIZE_STATE_NAME;
				break;
			}
			break;
		case NORMALIZE_STATE_NAME_DOT_DOT:
			if (path[src] == '/') {
				if (dst != 0) {
					dst -= 2;
					while (dst != 0 && path[dst - 1] != '/') {
						dst--;
					}
				}
				state = NORMALIZE_STATE_TRAILING_SLASH;
			} else {
				path[dst++] = '.';
				path[dst++] = '.';
				path[dst++] = path[src];
				state = NORMALIZE_STATE_NAME;
			}
			break;
		case NORMALIZE_STATE_NAME_DOT:
			switch (path[src]) {
			case '/':
				state = NORMALIZE_STATE_TRAILING_SLASH;
				break;
			case '.':
				state = NORMALIZE_STATE_NAME_DOT_DOT;
				break;
			default:
				path[dst++] = '.';
				path[dst++] = path[src];
				state = NORMALIZE_STATE_NAME;
				break;
			}
			break;
		case NORMALIZE_STATE_NAME:
			path[dst++] = path[src];
			if (path[src] == '/') {
				state = NORMALIZE_STATE_TRAILING_SLASH;
			}
			break;
		}
		src++;
	}

	switch (state) {
	case NORMALIZE_STATE_TRAILING_SLASH:
		/* fallthrough */
	case NORMALIZE_STATE_NAME_DOT:
		if (dst != 0) {
			dst--;
		}
		break;
	case NORMALIZE_STATE_NAME_DOT_DOT:
		if (dst != 0) {
			dst -= 2;
			while (dst != 0 && path[dst] != '/') {
				dst--;
			}
		}
		break;
	case NORMALIZE_STATE_NAME:
		break;
	}

	path[dst] = '\0';

	if (*path == '\0') {
		/* Empty, or resolved empty, is always invalid */
		return -1;
	}

	return 0;
}

static int
git_host_check_repository_path(const char *path, enum git_host_mode mode) {
	/* Only paths of the form <toplevel>/<git dir> are allowed to reference repositories */
	const char * const s = strchr(path, '/');

	if (s == NULL || s[1] == '.' || strchr(s + 1, '/') != NULL) {
		return -1;
	}

	if (mode & GIT_HOST_MODE_WR) {
		const char * const authorized = getenv("SSH_AUTHORIZED_BY");

		if (authorized == NULL) {
			errx(EXIT_FAILURE, "Missing authorization");
		}

		const size_t authorizedlen = strlen(authorized);
		if (authorizedlen != s - path
			|| strncmp(path, authorized, authorizedlen) != 0) {
			return -1;
		}
	}

	return 0;
}

static char *
git_host_repository(const char *raw, enum git_host_mode mode) {
	char path[strlen(raw) + 1];

	memcpy(path, raw, sizeof (path));
	if (git_host_normalize_path(path) != 0 || git_host_check_repository_path(path, mode) != 0) {
		errx(EXIT_FAILURE, "Invalid repository path '%s' '%s'", path, raw);
	}

	return git_host_pathcat(CONFIG_GIT_HOST_REPOSITORIES, path);
}

static int
git_host_dir_filter(const struct dirent *entry) {
	return *entry->d_name != '.';
}

static void
git_host_dir(const char *directory) {
	struct dirent **namelist;
	int count;

	count = scandir(directory, &namelist, git_host_dir_filter, alphasort);
	if (count < 0) {
		err(EXIT_FAILURE, "scandir %s", directory);
	}

	printf("%s:\n", directory);
	for (int i = 0; i < count; i++) {
		struct dirent * const entry = namelist[i];

		printf("\t%s/%s\n", directory, entry->d_name);

		free(entry);
	}

	free(namelist);
}

static void noreturn
git_host_exec_dir(int argc, char **argv) {

	if (chdir(CONFIG_GIT_HOST_REPOSITORIES) != 0) {
		err(EXIT_FAILURE, "chdir "CONFIG_GIT_HOST_REPOSITORIES);
	}

	if (argc == 1) {
		DIR * const dirp = opendir(".");
		struct dirent *entry;

		while (errno = 0, entry = readdir(dirp)) {
			if (*entry->d_name != '.') {
				git_host_dir(entry->d_name);
			}
		}

		closedir(dirp);
	} else {
		for (int i = 1; i < argc; i++) {
			if (*argv[i] != '.') {
				git_host_dir(argv[i]);
			}
		}
	}

	exit(EXIT_SUCCESS);
}

static void noreturn
git_host_exec_init(int argc, char **argv) {
	static const char argv0[] = "git-init";

	if (argc != 2) {
		fprintf(stderr, "usage: %s <repository>\n", *argv);
		exit(EXIT_FAILURE);
	}

	execl(git_host_execpath(argv0), argv0, "--quiet", "--bare", "--", git_host_repository(argv[1], GIT_HOST_MODE_WR), NULL);
	err(-1, "exec %s", argv0);
}

static void noreturn
git_host_exec_rx_tx(int argc, char **argv, enum git_host_mode mode) {

	if (argc != 2) {
		fprintf(stderr, "usage: %s <repository>\n", *argv);
		exit(EXIT_FAILURE);
	}

	execl(git_host_execpath(argv[0]), argv[0], git_host_repository(argv[1], mode), NULL);
	err(-1, "exec %s", *argv);
}

static void noreturn
git_host_exec_git_receive_pack(int argc, char **argv) {
	git_host_exec_rx_tx(argc, argv, GIT_HOST_MODE_WR);
}

static void noreturn
git_host_exec_git_upload_X(int argc, char **argv) {
	git_host_exec_rx_tx(argc, argv, GIT_HOST_MODE_RO);
}

static void noreturn
git_host_exec(int argc, char **argv) {
	static const struct {
		const char * const name;
		void (* const exec)(int, char **);
	} commands[] = {
		{ "dir",                git_host_exec_dir },
		{ "init",               git_host_exec_init },
		{ "git-receive-pack",   git_host_exec_git_receive_pack },
		{ "git-upload-archive", git_host_exec_git_upload_X },
		{ "git-upload-pack",    git_host_exec_git_upload_X },
	};
	const unsigned int commandscount = sizeof (commands) / sizeof (*commands);
	unsigned int i = 0;

	while (i < commandscount && strcmp(*argv, commands[i].name) != 0) {
		i++;
	}

	if (i == commandscount) {
		errx(EXIT_FAILURE, "Invalid command '%s'", *argv);
	}

	commands[i].exec(argc, argv);
	abort();
}

static void noreturn
git_host_usage(const char *progname) {
	fprintf(stderr, "usage: %s [-c <command>]\n", progname);
	exit(EXIT_FAILURE);
}

static const struct git_host_args
git_host_parse_args(int argc, char **argv) {
	struct git_host_args args = {
		.command = NULL,
	};
	int c;

	while (c = getopt(argc, argv, ":c:"), c != -1) {
		switch (c) {
		case 'c':
			args.command = optarg;
			break;
		case ':':
			warnx("-%c: Missing argument", optopt);
			git_host_usage(*argv);
		default:
			warnx("Unknown argument -%c", optopt);
			git_host_usage(*argv);
		}
	}

	if (args.command == NULL) {
		warnx("Missing command");
		git_host_usage(*argv);
	}

	if (optind != argc) {
		warnx("Invalid number of arguments, expected none, found %d", argc - optind);
		git_host_usage(*argv);
	}

	return args;
}

int
main(int argc, char *argv[]) {
	const struct git_host_args args = git_host_parse_args(argc, argv);
	char **arguments;
	int count;

	git_host_expand_command(args.command, &count, &arguments);
	git_host_exec(count, arguments);
}
