FROM swr.cn-north-4.myhuaweicloud.com/cj-docker/cangjie_ubuntu18_arm_kernel:v2.2 AS base-aarch64
ARG UID
RUN useradd --create-home -u $UID --non-unique --shell /bin/bash dev
USER dev

FROM swr.cn-north-4.myhuaweicloud.com/cj-docker/cangjie_ubuntu18_x86_kernel:v2.8 AS base-x64
ARG UID
RUN useradd --create-home -u $UID --non-unique --shell /bin/bash dev
USER dev

FROM base-aarch64 AS opencode-aarch64
USER dev
RUN mkdir -p /home/dev/.config/opencode /home/dev/.local/share/opencode
RUN touch /home/dev/.local/share/opencode/auth.json
RUN curl -fsSL https://opencode.ai/install | bash

FROM base-x64 AS opencode-x64
USER dev
RUN mkdir -p /home/dev/.config/opencode /home/dev/.local/share/opencode
RUN touch /home/dev/.local/share/opencode/auth.json
RUN curl -fsSL https://opencode.ai/install | bash

