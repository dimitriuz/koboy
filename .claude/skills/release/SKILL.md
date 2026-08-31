---
name: release
description: Cut a koboy release - read every commit since the last v* tag, write .github/RELEASE-NOTES.md from them, bump ./VERSION, run the gates, tag and push so the release workflow publishes. Use this whenever the user asks to release, cut a release, ship a version, publish a build, tag a version, or bump the version, and whenever they name a version number like "release 0.6.0" or "let's do v0.5.4" - even if they only say "ship it" or "make a release" with no number, in which case ask for one.
---

# Cutting a koboy release

A release is a tag push. Everything else — cross-building fourteen cores,
packaging, uploading — is `.github/workflows/release.yml`'s job, and it will not
start until the tag exists. So the work here is getting the repository into a
state the tag can honestly describe, and the notes are most of that.

## What a release touches

Three files, and no others unless the release itself changed them:

| File | Why |
|---|---|
| `./VERSION` | The release number, and the only place it is written. The Makefile, `tests/test_dist.sh` and the workflow's tag gate all read this file. |
| `.github/RELEASE-NOTES.md` | The GitHub release body, verbatim. Its top half is about one release; the "Installing" half is boilerplate. |
| `CLAUDE.md` / `README.md` | Only when the release's content makes a claim there stale. Never as ceremony. |

Two gates in the workflow will stop a tag that disagrees with itself: the tag
must equal `v$(cat VERSION)`, and the notes must contain the version string.
Both exist because the failure they catch is silent — a release named after
nothing, or last release's story attached to this release's archive.

## The version number

If the user gave one, use it. If not, **ask** — do not infer a bump from the
diff. The difference between a patch and a minor release is a judgement about
what users should expect, and only the maintainer makes it.

Offer the actual candidates rather than an open question: read
`git tag -l 'v*' | sort -V | tail -1`, and present the patch and minor bumps
from it alongside a one-line summary of what is in the release, so the choice is
informed. koboy has never used a major bump; do not offer one unless asked.

## Writing the notes

This is the part with judgement in it. The rest is mechanics.

Read every commit since the last tag:

```sh
prev=$(git tag -l 'v*' | sort -V | tail -1)
git log --format='%H%n%s%n%b%n---' "$prev"..HEAD
```

Read the **bodies**, not just the subjects. This project writes long commit
messages that explain why a change exists and what it cost; that reasoning is
the raw material for good notes and it is not recoverable from a subject line.
Merge commits carry nothing — skip them and read what they merged.

Then write for **someone who owns a Kobo and is deciding whether to download
this**. Not a changelog. The questions they have are: what is different, does it
affect me, and is it safe to install. Concretely:

- **Lead with what changed and who it is for.** If a release fixes something for
  one class of device, say which devices by name — an Aura H2O owner should not
  have to infer that "Phoenix protocol" means them.
- **Group by what the user experiences**, not by subsystem. Three commits to
  `input.c` that together mean "the touchscreen works on your model" are one
  item, not three.
- **Say what is NOT verified, in the same breath as the claim.** This project is
  strict about this and the notes must not be the place the discipline lapses.
  "Fixed on the Aura H2O" is a lie if nobody owns one; "should fix, untested on
  the hardware it is for, please report" is the truth and is also more likely to
  get the report that settles it.
- **Link the issue** if the release answers one.
- Leave the "Installing" section alone unless something about installing
  changed. It is boilerplate on purpose.

Do not write a bare list of commit subjects. If the release is genuinely small
and internal, two honest sentences beat a padded page.

**Check the pins.** `git diff "$prev"..HEAD -- scripts/pins.txt` — a changed pin
means a core was rebuilt from a different upstream, which invalidates that
system's rows in `TESTED.md` and belongs in the notes. Say so there too if
nobody has.

## The gates, before tagging

Run these and read the output. The tag is a promise and these are what back it.

```sh
make test        # every binary must say 0 failures
make lint        # must say "lint: clean"
bash tests/smoke_host.sh

export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make kobo                    # zero warnings, with the real Linaro 4.9
bash tests/test_dist.sh      # see the trap below
bash scripts/verify-core.sh
```

**`tests/test_dist.sh` skips its packaging half if the cross compiler is not on
`PATH`, and says `PASS` either way.** It prints `packaging SKIPPED` in the pass
line and that is the only sign. For a release the packaging assertions are the
point — the archive's contents, the BIOS ban, the size cap — so export the
toolchain path first and confirm the pass line does NOT say SKIPPED.

If a gate fails, stop and say so. Do not tag around it.

## Tag and push

```sh
printf '<version>\n' > VERSION      # then rewrite .github/RELEASE-NOTES.md
git add -A
git commit                          # "release: X.Y.Z -- <what it is for>"
git tag -a "vX.Y.Z" -m "koboy X.Y.Z -- <same>"
git push origin main
git push origin "vX.Y.Z"
```

The release commit's message follows the repository's own convention: a subject
saying what the release is *for*, then a body explaining it the way any other
commit here does. `git log --format=%B v0.5.3 -1` is the worked example.

Push `main` before the tag. A tag pointing at a commit no branch contains is
reachable but orphaned, and the next `git pull` on another machine will not
have it.

## After the push

The tag starts `release.yml`. Watch it — a release that fails to build is worse
than no release, because the tag already exists:

```sh
gh api "repos/dimitriuz/koboy/actions/runs?event=push&per_page=5" \
  --jq '.workflow_runs[] | select(.head_branch=="vX.Y.Z") | "\(.status) \(.html_url)"'
```

Poll until `completed`, then confirm what actually shipped rather than assuming:

```sh
gh api repos/dimitriuz/koboy/releases/tags/vX.Y.Z \
  --jq '{name, draft, assets: [.assets[] | "\(.name) \(.size)"]}'
```

Both archives must be there — `koboy-X.Y.Z.zip` and `koboy-probe-X.Y.Z.zip` —
and `draft` must be false. Report the release URL and the asset sizes.

Cores are cached per pinned commit, so a release that changed no pin rebuilds
none of them and finishes in a couple of minutes. A release that moved a pin
rebuilds that core, and FBNeo alone is the long pole — tens of minutes. Expect
the wait rather than assuming the run has hung.

## If something is wrong after the tag

Do not delete and re-push a tag someone may have fetched. Bump to the next patch
version and release again; the workflow is cheap and a moving tag is not.

The one thing that IS safe to change in place is the release body, since it is
just text on the release object:

```sh
gh release edit vX.Y.Z --notes-file .github/RELEASE-NOTES.md
```
