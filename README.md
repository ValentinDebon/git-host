# Git Host

Simplify git server management.

Git host is an infrastructure of configurations and binary executables around sshd(8) meant to integrate
the server's system users with the git(1) user usually used to handle repositories.

## Simplify system administration

Git's [guide on setting up a server](https://git-scm.com/book/en/v2/Git-on-the-Server-Setting-Up-the-Server) demonstrates how to manually create a git user to setup remote repositories.
With this default configuration, adding a new user and checking up if he/she doesn't mess with other's repositories might be a tedious and time-consuming process.

Git host simplifies the process by just letting you add a system user to a dedicated group (the `git` group).

## Setting up the server

You must create a git user and a git group first:
```
useradd -Ur -md /srv/git -s /usr/local/bin/git-host git
cp -R /usr/share/git-core/templates /srv/git
mkdir /srv/git/repositories
cat > /srv/git/.gitconfig <<EOF
[init]
	defaultBranch = master
	templateDir = ~/templates
EOF
```
You can put whatever you want in the repository templates, and/or in the gitconfig, that's up to you.

Now we must setup the sshd configuration for our new git user.
You might need to enable the `PermitUserEnvironment` configuration in your `/etc/ssh/sshd_config`
to allow `ssh-host-authorized-keys` to forward the authorizing user's name through the `SSH_AUTHORIZED_BY` environment variable.
Then, you'll need to create a `Match` rule for the `git` user, you can usually set up a dedicated configuration file in `/etc/ssh/sshd_config.d/99-git-host.conf`:
```
PermitUserEnvironment SSH_AUTHORIZED_BY

Match User git
	AuthorizedKeysCommand /usr/local/bin/ssh-host-authorized-keys -G git -t %t -- %k
	AuthorizedKeysCommandUser nobody
	PasswordAuthentication no
```
Obviously, you must replace `/usr/local` by the installation prefix of the git host binary executables, note that for the executables to work correctly,
sshd(8) expects *the executable files and every parent directory up to the filesystem root to be owned by root* (the last condition isn't clearly stated in the sshd\_config(5) manpages).

Now, let's add user to the git server, let's say we want to let our user roger and all its ssh-authorized remote users use the git infrastructure:
```
usermod -G git roger
```
And that's it! Upon connection, the `ssh-host-authorized-keys` utility will scan all git's group users .ssh/authorized\_keys for a corresponding key.
Then, the `git-host` is executed and receives the `SSH_AUTHORIZED_BY` environment variable, which allows implementation of basic user-related access-control.

Imagine our server is named bob, and roger sat up its public keys from an alice host. From the alice host, and inside a repository, roger could setup a remote the following way:
```
ssh git@bob new roger/repo
git remote add bob ssh://git@bob:/roger/repo
git push -u bob master
```
This would create a new bare repository at location `~git/repositories/roger/repo` on bob.
