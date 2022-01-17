/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

struct git_host_args {
	const char *command;
};

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
			case '\0': state = EXPAND_END; *dst = '\0'; git_host_array_push(strdup(arg), argcp, argvp); break;
			case '"':  state = EXPAND_QUOTE_DOUBLE; src++; break;
			case '\'': state = EXPAND_QUOTE_SINGLE; src++; break;
			case ' ':  state = EXPAND_SPACES; *dst = '\0'; git_host_array_push(strdup(arg), argcp, argvp); break;
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

static inline int
git_host_check_path(const char *path) {

	while (*path != '\0' && (*path != '/' || *++path != '.')) {
		path++;
	}

	return *path != '\0';
}

static char *
git_host_repository(const char *repository) {
	const char * const user = getenv("SSH_AUTHORIZED_BY");

	if (user == NULL || *user == '\0') {
		errx(EXIT_FAILURE, "Missing authorized user");
	}

	while (*repository == '/') {
		repository++;
	}

	if (git_host_check_path(repository) != 0) {
		errx(EXIT_FAILURE, "Invalid repository '%s': a path component starts with '.'", repository);
	}

	const size_t repositorylen = strlen(repository), userlen = strlen(user);
	if (repositorylen < userlen + 2 || strncmp(repository, user, userlen) != 0 || repository[userlen] != '/') {
		errx(EXIT_FAILURE, "Invalid repository '%s': path's first component isn't '%s'", repository, user);
	}

	static const char repositories[] = "repositories/";
	char * const host = malloc(sizeof (repositories) + repositorylen);
	if (host == NULL) {
		err(EXIT_FAILURE, "malloc");
	}

	memcpy(host, repositories, sizeof (repositories) - 1);
	memcpy(host + sizeof (repositories) - 1, repository, repositorylen + 1);

	return host;
}

static void
git_host_exec_transfer(int argc, char **argv) {

	if (argc != 2) {
		fprintf(stderr, "usage: %s <repository>\n", *argv);
		exit(EXIT_FAILURE);
	}

	execlp(*argv, *argv, git_host_repository(argv[1]), NULL);
}

static void
git_host_exec_new(int argc, char **argv) {

	if (argc != 2) {
		fputs("usage: new <repository>\n", stderr);
		exit(EXIT_FAILURE);
	}

	execlp("git", "git", "init", "-q", "--bare", "--", git_host_repository(argv[1]), NULL);
}

static void noreturn
git_host_exec(int argc, char **argv) {
	static struct {
		const char *name;
		void (*exec)(int, char **);
	} commands[] = {
		{ "git-receive-pack",   git_host_exec_transfer },
		{ "git-upload-pack",    git_host_exec_transfer },
		{ "git-upload-archive", git_host_exec_transfer },
		{ "new",                git_host_exec_new },
	};
	const unsigned int commandscount = sizeof (commands) / sizeof (*commands);
	unsigned int i = 0;

	while (i < commandscount && strcmp(*argv, commands[i].name) != 0) {
		i++;
	}

	if (i == commandscount) {
		errx(127, "Invalid command '%s'", *argv);
	}

	commands[i].exec(argc, argv);
	err(127, "exec %s", commands[i].name);
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
