#!/usr/bin/env python3
import os
import argparse
from colors import *

CUR_DIR = os.path.abspath(os.path.dirname(__file__))

def check_prereq():
    if get_pubkey() == None:
        print(Color.fg.purple + \
              "# Before creating a VM, please create a ssh key first. \n" + \
              "# If you do not know how to create a ssh key, please   \n" + \
              "# check https://bit.ly/1UiZIkj                         \n" + \
              Color.reset)
        return False
    return True

def get_pubkey():
    def pubkey_exist(pk):
        pn = os.path.join(os.environ['HOME'], '.ssh', pk)
        if os.path.isfile(pn):
            return True
        return False
    pk_names = ['authorized_keys',
                'id_rsa.pub',
                'id_ecdsa.pub',
                'id_ed25519.pub',
                'id_xmss.pub']
    for pk in pk_names:
        if pubkey_exist(pk):
            return pk
    return None

def vm_create(size, hostname, output):
    cmd_create = """\
set -e; \
virt-builder fedora-28 -o {output} --format qcow2 --arch x86_64 \
             --size {size} --hostname {hostname} \
    """
    pkgs = "vim,emacs,ctags,cscope,cpuid,gcc,gdb,git,htop,kernel-devel,numactl,python-devel,python3,readline-devel,tig,tmux,tree,net-tools"
    cmd = cmd_create.format(size=size, \
                            hostname=hostname, \
                            output=output)
    print(cmd)
    os.system(cmd)
    cmd_copy = """\
set -e; \
virt-sysprep -a {output} \
             --copy-in ~/.ssh/{pn}:/ \
             --copy-in {mnthost}:/ \
             --network \
             --update \
             --install "{pkgs}" \
             --selinux-relabel \
             --firstboot-command 'systemctl enable sshd; \
                                  useradd -m -p "" {username}; \
                                  gpasswd -a {username} wheel; \
                                  gpasswd -a {username} adm; \
                                  gpasswd -a {username} sudo; \
                                  mkdir -p /home/{username}/.ssh; \
                                  mv /mnt-host.sh /home/{username}; \
                                  mv /{pn} /home/{username}/.ssh/authorized_keys; \
                                  chown -R {username} /home/{username}; \
                                  chgrp -R {username} /home/{username}; \
                                  chmod -R 700 /home/{username}/.ssh'
    """
    cmd= cmd_copy.format(output=output, \
                         pn=get_pubkey(),
                         pkgs=pkgs, \
                         mnthost=os.path.join(CUR_DIR, 'mnt-host.sh'), \
                         username=os.environ['USER'])
    print(cmd)
    os.system(cmd)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(prog='vm-create.py')
    parser.add_argument('-s', '--size', default="8G",
                        help="disk size: default size is 8G")
    parser.add_argument('-n', '--hostname', default="vm",
                        help="hostname of a vm: default is 'vm'")
    parser.add_argument('-o', '--output', default="vm.qcow2",
                        help="generated VM image name: default is 'vm.qcow2'")

    args = parser.parse_args()
    if not check_prereq():
        exit(1)
    vm_create(args.size, args.hostname, args.output)
