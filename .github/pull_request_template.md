
# Pull Request Template

## Title
Please include the Jira Story Number in the format: `[ISSUE-123] Brief description of changes`

## Description
Link to Jira Story: [Add link here]

### Changes
- [ ] Feature implementation
- [ ] Bug fix
- [ ] Documentation update
- [ ] Other (please describe)

### Testing
- [ ] Unit tests added/updated
- [ ] Integration tests passed
- [ ] Manual testing completed

### Build Trigger Guidance (Build Ceph Deliverables)
- [ ] For PR build validation, add one label: `build-debian`, or `build-ubuntu`
- [ ] If label was not present at PR open time, add it now (adding a label triggers the workflow)
- [ ] For post-merge branch builds, ensure the target branch is `aka_version_*` (build runs on push after merge)
- [ ] For release builds, push a tag matching `v*`
- [ ] If PR checks show this workflow as skipped, confirm one of the build labels is present

### Checklist
- [ ] Code follows project style guidelines
- [ ] Self-review completed
- [ ] Comments added for complex logic
- [ ] Documentation updated
- [ ] No breaking changes (or documented if present)

### Related Issues
Closes [link to issue]
