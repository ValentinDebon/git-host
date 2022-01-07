/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdnoreturn.h>
#include <sys/types.h>
#include <unistd.h>
#include <syslog.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <err.h>

struct ssh_host_authorized_keys_args {
	const char *keytype;
	const char *group;
};

static const char * const sshd_authorized_keys_types[] = {
	"sk-ecdsa-sha2-nistp256@openssh.com",
	"ecdsa-sha2-nistp256",
	"ecdsa-sha2-nistp384",
	"ecdsa-sha2-nistp521",
	"sk-ssh-ed25519@openssh.com",
	"ssh-ed25519",
	"ssh-dss",
	"ssh-rsa",
};

static int
ssh_host_authorized_keys_skip_options(const char **entryp) {
	const struct authorized_keys_option {
		const char *name;
		enum {
			AUTHORIZED_KEYS_OPTION_SIMPLE,
			AUTHORIZED_KEYS_OPTION_ALLOW_NO,
			AUTHORIZED_KEYS_OPTION_WITH_ARGS,
		} type;
	} options[] = {
		{ "restrict", AUTHORIZED_KEYS_OPTION_SIMPLE },
		{ "cert-authority", AUTHORIZED_KEYS_OPTION_SIMPLE },

		{ "port-forwarding", AUTHORIZED_KEYS_OPTION_ALLOW_NO },
		{ "agent-forwarding", AUTHORIZED_KEYS_OPTION_ALLOW_NO },
		{ "x11-forwarding", AUTHORIZED_KEYS_OPTION_ALLOW_NO },
		{ "touch-required", AUTHORIZED_KEYS_OPTION_ALLOW_NO },
		{ "verify-required", AUTHORIZED_KEYS_OPTION_ALLOW_NO },
		{ "pty", AUTHORIZED_KEYS_OPTION_ALLOW_NO },
		{ "user-rc", AUTHORIZED_KEYS_OPTION_ALLOW_NO },

		{ "command", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
		{ "principals", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
		{ "from", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
		{ "expiry-time", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
		{ "environment", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
		{ "permitopen", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
		{ "permitlisten", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
		{ "tunnel", AUTHORIZED_KEYS_OPTION_WITH_ARGS },
	};
	const unsigned int optionscount = sizeof (options) / sizeof (*options);
	const char *entry = *entryp;

	while (*entry != '\0' && *entry != ' ' && *entry != '\t') {

		for (unsigned int i = 0; i < optionscount; i++) {
			const size_t optionlen = strlen(options[i].name);
			unsigned int skipped = 0;

			switch (options[i].type) {
			case AUTHORIZED_KEYS_OPTION_ALLOW_NO:
				if (strncasecmp(entry, "no-", 3) == 0) {
					skipped += 3;
				}
				/* fallthrough */
			case AUTHORIZED_KEYS_OPTION_SIMPLE:
				if (strncasecmp(entry + skipped, options[i].name, optionlen) == 0) {
					skipped += optionlen;
				} else {
					skipped = 0; /* rollback ALLOW_NO skipped */
				}
				break;
			case AUTHORIZED_KEYS_OPTION_WITH_ARGS:
				if (strncasecmp(entry, options[i].name, optionlen) == 0) {
					skipped += optionlen;

					if (entry[skipped] != '=') {
						return -1;
					}
					skipped++;

					if (entry[skipped] != '"') {
						return -1;
					}
					skipped++;

					while (entry[skipped] != '\0' && entry[skipped] != '"') {
						if (entry[skipped] == '\\' && entry[skipped + 1] == '"') {
							skipped++;
						}
						skipped++;
					}

					if (entry[skipped] == '\0') {
						return -1;
					}
					skipped++;
				}
				break;
			}

			if (skipped != 0) {
				entry += skipped;
				break;
			}
		}

		if (*entry == '\0' || *entry == ' ' || *entry == '\t') {
			break;
		}

		if (*entry != ',') {
			return -1;
		}
		entry++;

		if (*entry == '\0') {
			return -1;
		}
	}

	*entryp = entry;

	return 0;
}

static int
ssh_host_authorized_keys_entry_field_matches(const char *field, const char **entryp) {
	const size_t fieldlen = strlen(field);
	const char * const entry = *entryp;
	int ret = -1;

	if (strncmp(entry, field, fieldlen) == 0 && isblank(entry[fieldlen])) {
		*entryp = entry + fieldlen;
		ret = 0;
	}

	return ret;
}

static int
ssh_host_authorized_keys_user(const struct passwd *pw, const char *keytype, const char *key) {
	static const char authorizedkeysfile[] = "/.ssh/authorized_keys";
	const size_t homelen = strlen(pw->pw_dir);
	char authorizedkeys[homelen + sizeof (authorizedkeysfile)];
	int ret = -1;

	memcpy(authorizedkeys, pw->pw_dir, homelen);
	memcpy(authorizedkeys + homelen, authorizedkeysfile, sizeof (authorizedkeysfile));

	FILE * const filep = fopen(authorizedkeys, "r");
	if (filep != NULL) {
		char *line = NULL;
		size_t n = 0;
		ssize_t size;

		while (size = getline(&line, &n, filep), size >= 0) {
			const char *entry = line;

			if (*entry != '#' && *entry != '\n') {
				if (ssh_host_authorized_keys_skip_options(&entry) != 0 && entry != line) {
					/* Invalid options parsing */
					continue;
				}

				while (isblank(*entry)) {
					entry++;
				}

				if (ssh_host_authorized_keys_entry_field_matches(keytype, &entry) != 0) {
					/* Invalid keytype */
					continue;
				}

				while (isblank(*entry)) {
					entry++;
				}

				if (ssh_host_authorized_keys_entry_field_matches(key, &entry) != 0) {
					/* Invalid key */
					continue;
				}

				ret = 0;
				break;
			}
		}

		free(line);
		fclose(filep);
	}

	return ret;
}

static void noreturn
ssh_host_authorized_keys_usage(const char *progname) {
	fprintf(stderr, "usage: %s -G <group> -t <keytype> <key>\n", progname);
	exit(EXIT_FAILURE);
}

static const struct ssh_host_authorized_keys_args
ssh_host_authorized_keys_parse_args(int argc, char **argv) {
	struct ssh_host_authorized_keys_args args = {
		.keytype = NULL,
		.group = NULL,
	};
	int c;

	while (c = getopt(argc, argv, ":G:t:"), c != -1) {
		switch (c) {
		case 'G':
			args.group = optarg;
			break;
		case 't':
			args.keytype = optarg;
			break;
		case ':':
			warnx("-%c: Missing argument", optopt);
			ssh_host_authorized_keys_usage(*argv);
		default:
			warnx("Unknown argument -%c", optopt);
			ssh_host_authorized_keys_usage(*argv);
		}
	}

	if (args.group == NULL) {
		warnx("Expected an authorized users group, none specified");
		ssh_host_authorized_keys_usage(*argv);
	}

	if (args.keytype != NULL) {
		const unsigned int count = sizeof (sshd_authorized_keys_types) / sizeof (*sshd_authorized_keys_types);
		unsigned int k = 0;

		while (k < count && strcmp(sshd_authorized_keys_types[k], args.keytype) != 0) {
			k++;
		}

		if (k == count) {
			fprintf(stderr, "Invalid key type %s, expected one of:\n", args.keytype);
			for (unsigned int i = 0; i < count; i++) {
				fprintf(stderr, "\t- %s\n", sshd_authorized_keys_types[i]);
			}
			ssh_host_authorized_keys_usage(*argv);
		}
	} else {
		warnx("Expected key type, none specified");
		ssh_host_authorized_keys_usage(*argv);
	}

	if (argc - optind != 1) {
		warnx("Invalid number of arguments, expected 1, found %d", argc - optind);
		ssh_host_authorized_keys_usage(*argv);
	}

	return args;
}

int
main(int argc, char *argv[]) {
	const struct ssh_host_authorized_keys_args args = ssh_host_authorized_keys_parse_args(argc, argv);
	const char * const key = argv[optind];

	openlog("ssh-authorized-group", LOG_NDELAY | LOG_PERROR, LOG_USER);
	atexit(closelog);

	/* Find the group to get the list of authorized candidate users */
	struct group *gr;
	atexit(endgrent);

	while (errno = 0, gr = getgrent(), gr != NULL && strcmp(args.group, gr->gr_name) != 0);

	if (gr == NULL) {
		if (errno != 0) {
			syslog(LOG_ERR, "getgrent: %m");
			return EXIT_FAILURE;
		}
		syslog(LOG_INFO, "Unable to find a group named '%s' in the group entries", args.group);
		return EXIT_FAILURE;
	}

	/* Find the first user associated with the key */
	struct passwd *pw;
	atexit(endpwent);

	while (errno = 0, pw = getpwent(), pw != NULL) {
		const char * const *members = (const char * const *)gr->gr_mem;

		while (*members != NULL && strcmp(pw->pw_name, *members) != 0) {
			members++;
		}
	
		if (*members != NULL && ssh_host_authorized_keys_user(pw, args.keytype, key) == 0) {
			syslog(LOG_INFO, "Authorized key '%s' for user '%s' under group '%s'", key, pw->pw_name, gr->gr_name);
			break;
		}
	}

	if (pw == NULL) {
		if (errno != 0) {
			syslog(LOG_ERR, "getpwent: %m");
			return EXIT_FAILURE;
		}
		syslog(LOG_INFO, "Unable to find a user associated with the given key in the user entries");
		return EXIT_FAILURE;
	}

	/* Print authorized key entry, with user name in environment variables */
	printf("environment=\"SSH_AUTHORIZED_BY=%s\" %s %s\n", pw->pw_name, args.keytype, key);

	return EXIT_SUCCESS;
}
