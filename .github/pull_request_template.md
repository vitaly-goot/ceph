<!--
  - Please give your pull request a title like
    [component]: [short description]
-->

## Summary

<!-- Briefly describe what changed and why. -->

## Build / CI Notes

For pull requests targeting an `aka_version_*` branch:

- **Build Ceph Binaries** (`build-binaries.yaml`) — always runs
- **Build Ceph Debian Deliverables** (`build-debian.yaml`) — only runs if the PR
  has the `build-debian` label
- **Build Ceph Deliverables** (`build-deliverables.yaml`) — only runs if the PR
  has the `build-ubuntu` or `build-image` label. It builds the Debian packages. 
  If it has the `build-image` label, it then builds the Docker/build image from them

Add the relevant label when opening the PR, or add it afterward (which
re-triggers the workflow), if you need these to run.

These PR runs validate the build but do not publish it:

- Debian packages are uploaded as workflow artifacts, not published to Artifactory
- The Docker/build image is built but not pushed to the registry
- Publishing to Artifactory and the image registry only happens on non-PR refs
  (e.g. pushes to `aka_version_*`/`patch/*` or tags)

Note: some of these jobs run under a GitHub environment (`development`/`qa`/
`testing`) that may have its own approval or protection rules configured in
the repo settings; those aren't reflected here.

<!-- Add links when reviewers should inspect these outputs. -->
- Debian artifact run: <!-- link or N/A -->
- Build image run: <!-- link or N/A -->

## Testing

<!-- List tests performed, or explain why testing was not needed. -->

## Notes For Reviewers

<!-- Call out review priorities, risks, follow-up work, or anything else useful. -->



