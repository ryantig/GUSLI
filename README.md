<!--
SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->
# Gusli
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

## Description
Gusli (G3+ User Space Access Library) is a C++ library encapsulating IO (Read/Write) to any local block device (or a file).
It has a client library for IO submitting apps and server library for execution of the io's via backend
- Both client and server processes run on the same machine in user space
- Zero copy kernel bypass io.
- 1 client connects to multiple servers. Each server represents a block device.
- Supported servers: SPDK based apps with standard bdevs, Any other process based on server library examples.
- IO (read/Write) can be blocking or asyncronous. Async-io can use callback functions and/or be polled for completion and/or canceled.

Additional Documentation and description: [Here](https://docs.google.com/document/d/1xXLyA2Di2G04zfLy8dzMor9DC2PNqUuXhMEFA2ZYpO4)

## Installation & Usage
`make all`   Both g++ and clang++ are supported. clang example: `make USE_CLANG=1 all`
- executable of unitests will be created in root dir
`make clean all BUILD_RELEASE=1 BUILD_FOR_UNITEST=0 TRACE_LEVEL=5; ll /usr/lib/libg*; ll /usr/include/gus*;`
- use the command above to build and install production libraries into /usr  (/usr/lib, /usr/include)
- See examples directory for client/server implementation examples
- See unit-tests directory to learn how to use the library

`make help`   To see more info of how to compile

### Dependencies
- Mandatory: None. Plain C++ code (libc) code.
- Third party components are not auto downoaded/installed. Please do the following:

| OS      |            Install                      |   Remarks   |
|---------|----------------------------------------------|-------------|
| Ubuntu  | `apt install g++ make cmake pkg-config` | ✅          |
| Fedora  | `dnf install gcc-c++ cmake pkg-config`  | Not tested  |

- Optional:
    - For IO's with large scatter gathers (> 64 ranges), uring API to local blockdevices may help. So consider: `sudo apt install liburing-dev`
    - For developers: usefull utils: `sudo apt install -y build-essential gdb meld ncdu tree valgrind`;

### Monitor IO environment using GUSLI
Bash script which allows you to monitor the io buffers, stacks, threads, etc of both client and server processes, as well as force kill them.
1. `source gusli/80scripts/service.sh`;
2. `GUSLI;` To get help about about monitoring client and server

### Examples (C++):
1. Client: [write-read-verify io](./07examples/client/sample_client.hpp)
    - `class unitest_io` shows how io can be launched in many modes (with/without async callback, with/without polling, blocking mode), with verification.
2. Server: [A few server classes](./07examples/server/)
3. SPDK Both: Executable which forks itself, runs spdk server in 1 process, client basic io tests in another process [spdk_app](./unitest/sample_app_spdk_both.cpp)
4. `./unitest/run` for running various unitests

### Integrations:
- Integrating client with NIXL [install script](./80scripts/integration_with_nixl.sh)
- Integrating server with SPDK [install script](./80scripts/integration_with_spdkbdev.sh)

## Support
The current Maintainers Group for the project consists of:

| Maintainer              | GitHub ID                                             | Affiliation | Email               |
|-------------------------|-------------------------------------------------------|-------------|---------------------|
| Daniel Herman Shmulyan  | [danielhe](https://github.com/danielhe-nvidia)        | NVIDIA      | DanielHe@nvidia.com |

### Authors and acknowledgment
Show your appreciation to those who have contributed to the project.

## Contributing
Upon adding new features, make sure that unitest.cpp covers them and all unit-tests pass.

### Roadmap & Project status
1. [Jira](https://jirasw.nvidia.com/browse/NVMESH-5811)
2. ⚠️ Repository Todo
3. Todos in the code are marked with `nvTODO()` macro

> [!Important]
> Carefully read the below known issues

#### Large known issues ⚠️
1. Proper client server disconenct-reconnect while io is in air. Examples:
    - Unmap/Remap mem buffers destroys io buffers! Think how to solve it
    - Client disconnect while Server backend returning completions - dont free io and executor
    - Properly handling ghost remaining ios after cancel or disconnect
    - This harms server upradability. Server restarting while IO is in air will not resubmit the ios automatically.
2. Proper Design for IO canceling.
    - For now IO canceling is implemented as cancel and blocking wait for all after effects of io to finish. This can lead to unbounded wait. We need to better understand how users will use the cancel and design appropriately
    - Possible change: Dont block on canceled io but block on stop_all_ios() or unregister memory which those ios use.
3. Proper Design for IO done(). I introduced this destructor to force the user to actually think how they use the io api concurently as io may have multithreaded access (completion callback, polling thread, io cancel thread, io destructor call). Explicit call to done() requires the user to understand when his io is terminated. But this is not mandatory and can be taken care internally in the io implementation via atomic refcount + block the ~destructor(). This however has its own costs, races and requires a carefull planning
4. No dynamic addition of servers. When starting gusli library a full configuration of servers is given. Only subset of servers can be actually connected, but a new server which is not in the config, cannot be accessed by the client.
5. No support for timeouts. User can monitor the IO and call cancel, but this does not guarantee io finish, much like stuck io to a local nvme disk.

### Version release: CI for new version release, latest stable version:
1. Run Correctness tests (pass the following tests): `./unitest/run ci`;
2. Verify io performance did not suffer > 1.5[Miops]. Typically 2[M]+ for single core
3. Tag the version. Example: `git tag -a v0.04 -m "changes a, b, fix c"; git push --tags; git ls-remote --tags origin`
4. Update the [change log](./CHANGELOG.md)

### Github Todos:
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

