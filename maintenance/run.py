#!/usr/bin/env python3
"""Scheduled fork maintenance. Agents never touch the user's worktree; after an upstream sync this script only
fast-forwards the local main branch when that is safe."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path('/home/wilf/.local/share/hyprland-maintenance')
STATE = Path('/home/wilf/.local/state/hyprland-maintenance')
REPO = 'gfhdhytghd/Hyprland'
UPSTREAM = 'https://github.com/hyprwm/Hyprland.git'
LOCAL = Path('/home/wilf/data/Hyprland')
# Fork main needs the DRM color pipeline API from the aquamarine fork; flake.nix pins its master branch.
AQ_REPO = 'gfhdhytghd/aquamarine'
AQ_UPSTREAM = 'https://github.com/hyprwm/aquamarine.git'
AQ_LOCAL = Path('/home/wilf/data/aquamarine')
LOGFILE = STATE / 'maintenance.log'
MODE = '-'


def log(message):
    """Print for the journal and append to a persistent log; the user journal doesn't keep these runs."""
    print(message, flush=True)
    try:
        STATE.mkdir(parents=True, exist_ok=True)
        if LOGFILE.exists() and LOGFILE.stat().st_size > 5 * 1024 * 1024:
            LOGFILE.replace(LOGFILE.with_suffix('.log.1'))
        stamp = datetime.datetime.now().astimezone().isoformat(timespec='seconds')
        with LOGFILE.open('a') as output:
            for line in str(message).splitlines() or ['']:
                output.write(f'{stamp} [{MODE}] {line}\n')
    except OSError:
        pass


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=None).strip()


def api(endpoint):
    return json.loads(run('gh', 'api', endpoint))


def snapshot(item):
    number = item['number']
    path = f'repos/{REPO}'
    result = {'issue': api(f'{path}/issues/{number}'),
              'comments': json.loads(run('gh', 'api', '--paginate', '--slurp', f'{path}/issues/{number}/comments?per_page=100'))}
    if 'pull_request' in item:
        result['pull'] = api(f'{path}/pulls/{number}')
        for kind in ('reviews', 'comments'):
            result[kind + '_pr'] = json.loads(run('gh', 'api', '--paginate', '--slurp', f'{path}/pulls/{number}/{kind}?per_page=100'))
    return result


def notify(summary, body='', urgency='normal'):
    # Best effort: the timer runs in the user session, but there may be no notification daemon.
    subprocess.run(['notify-send', '--app-name=Hyprland fork', f'--urgency={urgency}', summary, body[:500]],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def workspace(label, slug=REPO, upstream=UPSTREAM, branch='main', reference=LOCAL):
    target = STATE / 'runs' / (datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + label)
    target.mkdir(parents=True)
    repo = target / 'repo'
    run('git', 'clone', '--reference-if-able', str(reference),
        '--dissociate', '--single-branch', '--branch', branch, f'https://github.com/{slug}.git', str(repo))
    run('git', 'remote', 'add', 'upstream', upstream, cwd=repo)
    run('git', 'config', 'user.name', 'gfhdhytghd', cwd=repo)
    run('git', 'config', 'user.email', '3391146750@qq.com', cwd=repo)
    return target, repo


def claude(target, repo, task):
    prompt = (ROOT / 'instructions.txt').read_text() + '\n\nTASK:\n' + task
    (target / 'prompt.txt').write_text(prompt)
    with (target / 'claude.json').open('w') as output:
        subprocess.run(['claude', '-p', '--model', 'opus', '--output-format', 'json',
                        '--permission-mode', 'acceptEdits', '--permission-prompts', 'none',
                        '--allowedTools', 'Bash,Read,Write,Edit,Glob,Grep,Agent', '--', prompt],
                       cwd=repo, stdout=output, check=True, timeout=18000)
    result = json.loads((target / 'claude.json').read_text())
    log(f"agent run {target.name}: cost=${result.get('total_cost_usd') or 0:.2f} turns={result.get('num_turns')} error={result.get('is_error')}")
    if result.get('is_error') or result.get('permission_denials'):
        log(result.get('result', ''))
        raise RuntimeError(f'Claude failed or encountered denied tools; inspect {target}')
    log(result.get('result', ''))
    receipt = json.loads((target / 'receipt.json').read_text())
    log(f'receipt {target.name}: {receipt}')
    if receipt.get('status') != 'completed':
        raise RuntimeError(f'Incomplete task; preserved for retry: {target}')


def update_local(remote_main, local_repo=LOCAL, slug=REPO, branch='main'):
    """Fast-forward the user's local branch to the published fork branch, never discarding local work."""
    if not (local_repo / '.git').exists():
        return 'no local checkout'
    remote_name = next((name for name in run('git', 'remote', cwd=local_repo).split()
                        if run('git', 'remote', 'get-url', name, cwd=local_repo).rstrip('/').removesuffix('.git').endswith(slug)), None)
    if not remote_name:
        return f'no remote for {slug} in {local_repo}'
    run('git', 'fetch', '--quiet', remote_name, branch, cwd=local_repo)
    local = run('git', 'rev-parse', f'refs/heads/{branch}', cwd=local_repo)
    if local == remote_main:
        return f'local {branch} already up to date'
    if subprocess.run(['git', 'merge-base', '--is-ancestor', local, remote_main], cwd=local_repo).returncode != 0:
        return f'local {branch} {local[:9]} has commits not on {remote_name}/{branch}; left untouched'
    current = run('git', 'branch', '--show-current', cwd=local_repo)
    checkouts = [line for line in run('git', 'worktree', 'list', '--porcelain', cwd=local_repo).splitlines()
                 if line == f'branch refs/heads/{branch}']
    if checkouts and current != branch:
        return f'{branch} is checked out in another worktree; left untouched'
    if current == branch:
        if run('git', 'status', '--porcelain', '--untracked-files=no', cwd=local_repo):
            return f'{branch} is checked out with uncommitted changes; left untouched'
        run('git', 'merge', '--ff-only', '--quiet', remote_main, cwd=local_repo)
    else:
        run('git', 'update-ref', f'refs/heads/{branch}', remote_main, local, cwd=local_repo)
    return f'local {branch} fast-forwarded {local[:9]}..{remote_main[:9]}'


def head(url, ref):
    return run('git', 'ls-remote', url, ref).split()[0]


def mirror_tags(repo):
    """Push upstream tags the fork lacks (release version tags); never moves a tag the fork already has."""
    run('git', 'fetch', '--quiet', '--no-tags', 'upstream', '+refs/tags/*:refs/tags/*', cwd=repo)
    theirs = {line.split()[1] for line in run('git', 'ls-remote', '--tags', 'upstream', cwd=repo).splitlines()
              if not line.endswith('^{}')}
    ours = {line.split()[1] for line in run('git', 'ls-remote', '--tags', 'origin', cwd=repo).splitlines()
            if not line.endswith('^{}')}
    missing = sorted(theirs - ours)
    if not missing:
        return
    result = subprocess.run(['git', 'push', '--quiet', 'origin', *missing], cwd=repo)
    log(f"{repo}: mirrored tags {', '.join(tag.removeprefix('refs/tags/') for tag in missing)}"
        + ('' if result.returncode == 0 else ' (push failed)'))


def sync_aquamarine(check):
    """Merge upstream aquamarine main into the fork's master, which Hyprland fork main builds against."""
    upstream = head(AQ_UPSTREAM, 'refs/heads/main')
    fork = head(f'https://github.com/{AQ_REPO}.git', 'refs/heads/master')
    log(f'aquamarine upstream={upstream} fork={fork}')
    if check:
        return fork
    target, repo = workspace('aq-sync', AQ_REPO, AQ_UPSTREAM, 'master', AQ_LOCAL)
    run('git', 'fetch', 'upstream', 'main', cwd=repo)
    # Keep the fork's main branch a plain mirror of upstream; a non fast-forward push just fails and is logged.
    if subprocess.run(['git', 'push', 'origin', f'{upstream}:refs/heads/main'], cwd=repo).returncode != 0:
        log('aquamarine: could not fast-forward fork main to upstream main')
    mirror_tags(repo)
    if subprocess.run(['git', 'merge-base', '--is-ancestor', upstream, 'HEAD'], cwd=repo).returncode == 0:
        log('aquamarine: all upstream main commits are already integrated.')
        return fork
    claude(target, repo, f'''Synchronize ALL commits from hyprwm/aquamarine upstream/main through {upstream} into
origin/master of the fork {AQ_REPO} (this checkout). The fork's master carries DRM color pipeline / HDR
metadata work that gfhdhytghd/Hyprland main depends on; its flake.nix pins {AQ_REPO}/master. Merge the
upstream main history; do not cherry-pick. Resolve conflicts preserving the fork's API and fixes
The fork follows upstream's version: VERSION and the library SOVERSION must end up exactly as upstream
main has them (the fork keeps no version of its own); fix anything that depends on the old number.
Build aquamarine and run its tests. Install it into a prefix inside {target} (never a system path, never
/home/wilf/data/aquamarine/.prefix), then clone https://github.com/{REPO}.git main into {target} and verify
it configures, builds and passes unit tests against that prefix (PKG_CONFIG_PATH). If Hyprland fork main
no longer builds because of upstream aquamarine API changes, do not publish; preserve the merge on a named
branch of {AQ_REPO} and report the blocker. Push master normally (no force). Do not push to {REPO};
the Hyprland sync step updates its flake.lock afterwards.
After successful push, write {target}/receipt.json with {{"status":"completed"}}.''')
    remote = run('git', 'ls-remote', 'origin', 'refs/heads/master', cwd=repo).split()[0]
    run('git', 'fetch', 'origin', 'master', cwd=repo)
    run('git', 'merge-base', '--is-ancestor', upstream, remote, cwd=repo)
    try:
        local = update_local(remote, AQ_LOCAL, AQ_REPO, 'master')
    except subprocess.CalledProcessError as error:
        local = f'local update failed: {error}'
    log(f'aquamarine fork master {remote[:9]} published; {local}.')
    notify('Upstream aquamarine synced into fork', f'master {remote[:9]}; {local}; reinstall .prefix if you build against it')
    return remote


def pinned_aquamarine(repo):
    lock = json.loads((repo / 'flake.lock').read_text())
    return lock['nodes']['aquamarine']['locked'].get('rev')


def sync(check):
    # aquamarine first: Hyprland fork main builds against the fork's aquamarine master.
    aq_error = None
    try:
        aq_master = sync_aquamarine(check)
    except Exception as error:
        aq_error = f'aquamarine sync failed: {type(error).__name__}: {error}'
        log(aq_error)
        notify('aquamarine fork sync needs attention', aq_error, 'critical')
        aq_master = head(f'https://github.com/{AQ_REPO}.git', 'refs/heads/master')
    upstream = head(UPSTREAM, 'refs/heads/main')
    fork = head(f'https://github.com/{REPO}.git', 'refs/heads/main')
    log(f'upstream={upstream} fork={fork}')
    if check:
        return
    target, repo = workspace('sync')
    run('git', 'fetch', 'upstream', 'main', cwd=repo)
    mirror_tags(repo)
    pinned = pinned_aquamarine(repo)
    stale_pin = pinned != aq_master
    if subprocess.run(['git', 'merge-base', '--is-ancestor', upstream, 'HEAD'], cwd=repo).returncode == 0 and not stale_pin:
        log('All upstream main commits are already integrated.')
        if aq_error:
            raise RuntimeError(aq_error)
        return
    pin_task = (f'''
Also update the flake.lock `aquamarine` input from {pinned} to {AQ_REPO} master {aq_master}
(`nix flake update aquamarine` if nix is available, otherwise edit the locked rev/narHash/lastModified
consistently, getting narHash via `nix flake prefetch` or GitHub). Before publishing, build and unit-test
this main against aquamarine {aq_master} installed into a prefix inside {target} (never a system path).
If they are incompatible, keep the old pin, still publish the upstream merge if it is verified,
and report the blocker.''' if stale_pin else '')
    claude(target, repo, f'''Synchronize ALL commits from upstream/main through {upstream} into origin/main.{pin_task}
This includes every PR merged upstream, plus direct main commits. Version numbers follow upstream:
VERSION and dependency minimum versions (e.g. AQUAMARINE_MINIMUM_VERSION) take upstream's values, raised
only where fork main truly needs newer. Merge the upstream main history;
do not independently cherry-pick PRs, tags, or unmerged branches. Resolve conflicts preserving fork policy,
fork-specific features, and fixes. Build and run applicable tests before publishing. Push main normally
(no force). If remote main moves, fetch and incorporate its changes, then revalidate. If correctness
cannot be established, preserve work on a named branch and report the blocker; do not publish broken main.
After the push, the automation itself fast-forwards the user's local checkout; do not touch it yourself.
After successful push, write {target}/receipt.json with {{"status":"completed"}}.''')
    remote = run('git', 'ls-remote', 'origin', 'refs/heads/main', cwd=repo).split()[0]
    run('git', 'fetch', 'origin', 'main', cwd=repo)
    run('git', 'merge-base', '--is-ancestor', upstream, remote, cwd=repo)
    try:
        local = update_local(remote)
    except subprocess.CalledProcessError as error:
        local = f'local update failed: {error}'
    log(f'Fork main {remote[:9]} published; {local}.')
    notify('Upstream synced into fork', f'main {remote[:9]}; {local}')
    if aq_error:
        raise RuntimeError(aq_error)


def triage(check):
    pages = json.loads(run('gh', 'api', '--paginate', '--slurp', f'repos/{REPO}/issues?state=open&per_page=100'))
    items = [item for page in pages for item in page]
    statefile = STATE / 'handled.json'
    handled = json.loads(statefile.read_text()) if statefile.exists() else {}
    errors = []
    for item in items:
        key = str(item['number'])
        data = snapshot(item)
        fingerprint = digest(data)
        if handled.get(key) == fingerprint:
            continue
        log(f'Needs handling: #{key} {item["title"]}')
        if check:
            continue
        try:
            target, repo = workspace('item-' + key)
            (target / 'github-context.json').write_text(json.dumps(data, indent=2))
            claude(target, repo, f'''Handle fork item https://github.com/{REPO}/issues/{key}.
Read the full current issue/PR, comments, reviews and checks using gh; initial snapshot is in
{target}/github-context.json. You lead the work and may call Codex for implementation and independent
review. For an issue: investigate, fix reproducible bugs, add suitable tests and create a fork PR;
answer questions or ask for missing evidence in the issue.
When the issue is uncertain (unclear or unconfirmed root cause, a feature request or behaviour change
with several reasonable designs, a fix that needs a policy or config decision, or one that would
change existing behaviour), do NOT implement yet: think through the plausible causes and 2-3 concrete
implementation options with their trade-offs, post them in the issue and ask the reporter/maintainer
which direction to take or for the evidence needed to decide. That reply counts as handled; implement
once someone answers. For a PR: review correctness, regressions,
performance and tests; fix or request changes as appropriate, and merge when ready and verified.
You are authorized to comment, create/update/merge PRs and close resolved issues in THIS FORK.
Avoid duplicate fixes/PRs; check existing work first. Never claim hardware or visual verification
without evidence. A substantive response requesting required information counts as handled until
new activity. A transient tool/build/auth failure does not: record blocked instead for retry.
Before completing, re-read the item to catch changes that arrived during your work and address them.
Write {target}/receipt.json with {{"status":"completed"}} only after handling is complete;
otherwise use {{"status":"blocked","reason":"..."}}. Summarize actions and validation.''')
            handled[key] = digest(snapshot(item))
            notify(f'Fork #{key} handled', item['title'])
            temp = statefile.with_suffix('.tmp')
            temp.write_text(json.dumps(handled, indent=2))
            temp.replace(statefile)
        except Exception as error:
            errors.append(f'#{key}: {error}')
            log(errors[-1])
    log(f'Checked {len(items)} open issues/PRs.')
    if errors:
        notify('Fork triage needs attention', '; '.join(errors), 'critical')
        raise RuntimeError('; '.join(errors))


def notifications(check):
    """Run triage as soon as GitHub reports activity on the fork, instead of waiting for the timer."""
    threads = json.loads(run('gh', 'api', '--paginate', '--slurp', f'repos/{REPO}/notifications?all=false&per_page=50'))
    threads = [thread for page in threads for thread in page]
    log(f'{len(threads)} unread fork notification(s).')
    if not threads or check:
        return
    for thread in threads:
        log(f"  {thread['reason']}: {thread['subject']['type']} {thread['subject']['title']}")
    try:
        triage(False)
    finally:
        # Failed items are retried by the regular triage timer, not every notification poll.
        for thread in threads:
            run('gh', 'api', '--method', 'PATCH', f"notifications/threads/{thread['id']}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['sync', 'triage', 'notifications'])
    parser.add_argument('--check', action='store_true', help='Read-only discovery; never launch agents')
    args = parser.parse_args()
    MODE = args.mode + (' --check' if args.check else '')
    STATE.mkdir(parents=True, exist_ok=True)
    with (STATE / 'maintenance.lock').open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            log('Another maintenance run is active; next timer will retry.')
            raise SystemExit(0)
        try:
            {'sync': sync, 'triage': triage, 'notifications': notifications}[args.mode](args.check)
        except Exception as error:
            log(f'FAILED: {type(error).__name__}: {error}')
            raise
