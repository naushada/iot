FROM ubuntu:jammy
RUN apt-get update && apt-get -y upgrade && \
    apt-get install -y gcc g++ git make cmake libssl-dev zlib1g-dev libreadline-dev python3-dev

# Building openssl 3.1.1
RUN git clone -b openssl-3.1.1 https://github.com/naushada/openssl.git

RUN cd openssl && \
    gunzip openssl-3.1.1.tar.gz && \
    tar -xvf openssl-3.1.1.tar && \
    cd openssl-3.1.1 && \
    ./config --prefix=/usr/local/openssl-3.1.1 && \
    make && make install

# RUN openssl req -newkey rsa:4096  -x509  -sha512  -days 365 -nodes -out /opt/xAPP/cert/cert.pem -keyout /opt/xAPP/cert/key.pem

WORKDIR /root/mongo-c
# #RUN apt-get -y install mongodb-server-core
RUN git clone -b r1.19 https://github.com/mongodb/mongo-c-driver.git

RUN cd mongo-c-driver
WORKDIR /root/mongo-c/mongo-c-driver/build
RUN cmake .. && \
    make && make install

WORKDIR /root/mongo-cxx
RUN git clone -b releases/v3.6 https://github.com/mongodb/mongo-cxx-driver.git
RUN cd mongo-cxx-driver

WORKDIR /root/mongo-cxx/mongo-cxx-driver/build
RUN cmake .. -DBSONCXX_POLY_USE_MNMLSTC=1 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
RUN make && make install
RUN ldconfig

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


#FROM ubuntu:jammy
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

RUN cp /tmp/source/apps/build/lwm2m /opt/app/lwm2m
RUN cp /tmp/source/apps/build/test/lwm2m_test /opt/app/lwm2m_test

#COPY --from=compile-image /tmp/source/apps/build/lwm2m /opt/app/lwm2m
#COPY --from=compile-image /tmp/source/apps/build/test/lwm2m_test /opt/app/lwm2m_test

CMD "/opt/app/lwm2m"
# EXPOSE 22
# CMD ["/usr/sbin/sshd", "-D"]