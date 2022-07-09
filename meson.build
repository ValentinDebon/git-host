project('git-host', 'c', license : 'BSD-3-Clause')

gitexecpath = get_option('gitexecpath')
if gitexecpath == ''
	gitexecpath = get_option('prefix') / get_option('libdir') / 'git-core'
endif

configuration = configuration_data()
configuration.set('CONFIG_GIT_EXEC_PATH', gitexecpath, description: 'Directory to search for git executables')
configuration.set('CONFIG_GIT_HOST_REPOSITORIES', get_option('githostrepositories'), description: 'Git user home directory repositories store')

subdir('src/git-host')
subdir('src/ssh-host-authorized-keys')
