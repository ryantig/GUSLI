# Gusli

## Description
Gusli (G3+ User Space Access Library) is a C++ library encapsulating IO (Read/Write) to any local block device (or a file)
Documentation and description: https://docs.google.com/document/d/1xXLyA2Di2G04zfLy8dzMor9DC2PNqUuXhMEFA2ZYpO4

## Installation & Usage
`make all;`   Both g++ and clang++ are supported. clang example: `make USE_CLANG=1 all`
- executable of unitests will be created in root dir
`make clean all BUILD_RELEASE=1 BUILD_FOR_UNITEST=0 TRACE_LEVEL=5; ll /usr/lib/libg*; ll /usr/include/gus*;`
- use the command above to build and install production libraries into /usr  (/usr/lib, /usr/include)
- See examples directory for client/server implementation examples
- See unit-tests directory to learn how to use the library

## Dependencies
 * Mandatory: None. Plain C++ code (libc) code.
 * Optional: For IO's with large scatter gathers (> 64 ranges), uring API to local blockdevices may help. So consider: sudo apt install liburing-dev
## Support
The current Maintainers Group for the project consists of:

| Maintainer              | GitHub ID                                             | Affiliation | Email               |
|-------------------------|-------------------------------------------------------|-------------|---------------------|
| Daniel Herman Shmulyan  | [danielhe](https://github.com/danielhe-nvidia)        | NVIDIA      | DanielHe@nvidia.com |

## Roadmap & Project status
In development https://jirasw.nvidia.com/browse/NVMESH-5811

## Todos:
- [ ] [Set up project integrations](https://gitlab-master.nvidia.com/excelero/gusli/-/settings/integrations)
- [ ] [Invite team members and collaborators](https://docs.gitlab.com/ee/user/project/members/)
- [ ] [Create a new merge request](https://docs.gitlab.com/ee/user/project/merge_requests/creating_merge_requests.html)
- [ ] [Automatically close issues from merge requests](https://docs.gitlab.com/ee/user/project/issues/managing_issues.html#closing-issues-automatically)
- [ ] [Enable merge request approvals](https://docs.gitlab.com/ee/user/project/merge_requests/approvals/)
- [ ] [Set auto-merge](https://docs.gitlab.com/ee/user/project/merge_requests/merge_when_pipeline_succeeds.html)
- [ ] [Get started with GitLab CI/CD](https://docs.gitlab.com/ee/ci/quick_start/index.html)
- [ ] [Analyze your code for known vulnerabilities with Static Application Security Testing (SAST)](https://docs.gitlab.com/ee/user/application_security/sast/)
- [ ] [Deploy to Kubernetes, Amazon EC2, or Amazon ECS using Auto Deploy](https://docs.gitlab.com/ee/topics/autodevops/requirements.html)
- [ ] [Use pull-based deployments for improved Kubernetes management](https://docs.gitlab.com/ee/user/clusters/agent/)
- [ ] [Set up protected environments](https://docs.gitlab.com/ee/ci/environments/protected_environments.html)

***

## Contributing
Upon adding new features, make sure that unitest.cpp covers them and all unit-tests pass.

## Authors and acknowledgment
Show your appreciation to those who have contributed to the project.

## License
Nvidia Internal
