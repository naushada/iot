FROM ubuntu as compile-image
RUN apt-get update && apt-get -y upgrade && \
    apt-get install -y gcc g++ git make cmake libssl-dev zlib1g-dev libreadline-dev

WORKDIR /tmp
RUN git clone https://github.com/google/googletest/ && \
    cd /tmp/googletest && \
    mkdir build && cd build && cmake .. && make install &&\
    cd .. &&\
    mkdir source && \
    cd source

COPY apps /tmp/source/apps

RUN cd /tmp/source/apps &&\
    mkdir build &&\
    cd build &&\
    cmake .. && make


FROM ubuntu:jammy
RUN apt-get update && apt-get -y upgrade && \
    apt-get install -y gcc g++ git make cmake patch gdb openssh-server bash vim

# SSH server configuration
# TODO: SECURITY: here we enable root login WITHOUT PASSWORD in the container, useful for dev use case
# To be changed / removed for field deployment
RUN mkdir -p /root/.ssh \
    && chmod 0700 /root/.ssh \
    && ssh-keygen -A \
    && echo -e "PasswordAuthentication yes" >> /etc/ssh/sshd_config \
    && echo -e "PermitRootLogin yes" >> /etc/ssh/sshd_config \
    && echo -e "PermitEmptyPasswords yes" >> /etc/ssh/sshd_config \
    && passwd -d root

WORKDIR /opt/app

COPY --from=compile-image /tmp/source/apps/build/lwm2mClient /opt/app/lwm2mClient
#COPY --from=compile-image /tmp/source/apps/build/test/lwm2mClient_test /opt/app/lwm2mClient_test

CMD "/opt/app/lwm2mClient"