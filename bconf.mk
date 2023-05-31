CFLAGS+=-std=c11
CPPFLAGS+=-D_DEFAULT_SOURCE

src/git-host.o: CPPFLAGS+= \
	-DCONFIG_GIT_EXEC_PATH='"$(CONFIG_GIT_EXEC_PATH)"' \
	-DCONFIG_GIT_HOME_REPOSITORIES='"$(CONFIG_GIT_HOME_REPOSITORIES)"'

git-host: src/git-host.o
ssh-host-authorized-keys: src/ssh-host-authorized-keys.o

host-libexec+=git-host ssh-host-authorized-keys
clean-up+=$(host-libexec) $(host-libexec:%=src/%.o)
