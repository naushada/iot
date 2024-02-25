#ifndef __main_cpp__
#define __main_cpp__

//#include "cbor_adapter.hpp"
#include "app.hpp"

int main(std::int32_t argc, char *argv[])
{
    #if 0
    CBORAdapter cborInst;
    std::string out;
    cborInst.getCBOR("20240219085111_template_XR90.json", out);
    #endif

    //App app("coaps://10.203.77.36");
    App app("coaps://0.0.0.0");
    app.init();
    
    auto& adapter = app.get_adapter();
    std::string identity("97554878B284CE3B727D8DD06E87659A"), secret("3894beedaa7fe0eae6597dc350a59525");
    adapter.add_credential(identity, secret);
    // install signal handler
    struct sigaction sa;
    //sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGPIPE,  &sa, NULL);

    // prepare signal masks
    sigset_t signalMask;
    sigemptyset(&signalMask);
    sigaddset(&signalMask, SIGTERM);
    sigaddset(&signalMask, SIGPIPE);
    sigset_t emptyMask;
    sigemptyset(&emptyMask);
    
    app.start();
    
}





#endif /* __main_cpp__*/