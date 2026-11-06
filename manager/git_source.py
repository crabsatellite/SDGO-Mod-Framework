"""Credential-free Git source descriptors; authentication stays in local Git."""
from urllib.parse import urlsplit,urlunsplit
import re


def parse(value):
    if not isinstance(value,str)or len(value)>2048:raise ValueError('Git 更新地址无效')
    p=urlsplit(value);ref=p.fragment or 'main'
    if p.scheme not in('git+https','git+ssh')or not p.hostname or p.password or p.query:
        raise ValueError('使用 git+https:// 或 git+ssh:// 更新地址，不在地址中保存凭据')
    if p.username and(p.scheme!='git+ssh'or p.username!='git'):raise ValueError('更新地址不能包含私人账号')
    if(not re.fullmatch('[A-Za-z0-9][A-Za-z0-9_./-]{0,99}',ref)or'..'in ref or'//'in ref or
       ref.endswith(('/','.'))or any(x.startswith('.')or x.endswith('.lock')for x in ref.split('/'))):
        raise ValueError('Git 更新分支无效')
    if not p.path or p.path=='/':raise ValueError('Git 更新仓库路径无效')
    return urlunsplit((p.scheme[4:],p.netloc,p.path,'','')),ref


def environment():
    import os
    # Keep configured SSH commands/identity agents intact. Replacing them with
    # plain ssh can discard the very credentials that made the source readable.
    return {**os.environ,'GIT_TERMINAL_PROMPT':'0','GCM_INTERACTIVE':'Never','SSH_ASKPASS_REQUIRE':'never'}


def failure(stderr):
    text=stderr.decode('utf-8',errors='replace').casefold()if isinstance(stderr,bytes)else str(stderr).casefold()
    if any(s in text for s in('permission denied','authentication failed','could not read username','repository not found')):
        return '仓库不存在或当前 Git 身份没有读取权限；请检查对应 HTTPS 凭据或已有 SSH 权限'
    if any(s in text for s in('timed out','timeout','could not resolve','unable to access','could not read from remote')):
        return '连接更新仓库失败，请检查该源的地址和连接状态'
    return 'Git 未完成读取，请检查本机 Git 与所选仓库权限'
