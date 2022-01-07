/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <wordexp.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

struct git_host_args {
	const char *command;
};

static void
git_host_parse(const char *command, int *countp, char ***argumentsp) {
	char **arguments;
	wordexp_t p;

	switch (wordexp(command, &p, WRDE_NOCMD | WRDE_UNDEF)) {
	case 0:
		break;
	case WRDE_BADCHAR:
		errx(EXIT_FAILURE, "wordexp '%s': Illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }.", command);
	case WRDE_BADVAL:
		errx(EXIT_FAILURE, "wordexp '%s': An undefined shell variable was referenced, and the WRDE_UNDEF flag told us to consider this an error.", command);
	case WRDE_CMDSUB:
		errx(EXIT_FAILURE, "wordexp '%s': Command substitution requested, but the WRDE_NOCMD flag told us to consider this an error.", command);
	case WRDE_NOSPACE:
		errx(EXIT_FAILURE, "wordexp '%s': Out of memory.", command);
	case WRDE_SYNTAX:
		errx(EXIT_FAILURE, "wordexp '%s': Shell syntax error, such as unbalanced parentheses or unmatched quotes.", command);
	default:
		errx(EXIT_FAILURE, "wordexp '%s': Unknown error.", command);
	}

	if (p.we_wordc == 0) {
		errx(EXIT_FAILURE, "Nothing expanded in command '%s'", command);
	}

	arguments = malloc((p.we_wordc + 1) * sizeof (*arguments));
	for (unsigned int i = 0; i < p.we_wordc; i++) {
		arguments[i] = strdup(p.we_wordv[i]);
	}
	arguments[p.we_wordc] = NULL;

	wordfree(&p);

	*countp = p.we_wordc;
	*argumentsp = arguments;
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

	git_host_parse(args.command, &count, &arguments);
	git_host_exec(count, arguments);
}
