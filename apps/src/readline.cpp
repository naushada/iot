#ifndef __readline_cpp__
#define __readline_cpp__

extern "C" {
    #include <stdio.h>
    #include <string.h>
    #include <readline/readline.h>
    #include <readline/history.h>
}

#include "readline.hpp"

/*These are static data member*/
int Readline::m_offset = 0;
int Readline::m_len = 0;
int Readline::m_argOffset = 0;

/*This static data member stores the selected command*/
std::string Readline::m_cmdName;

std::vector<Readline::command> Readline::commands = {
  /*command name*/   /*command argument(s)*/                                                                             /*command description*/ 
  { "post",          {{"data"}, {"uri"}, {"file"}, {"content-format"}, {"uri-query"}, {"is-confirmable=true"}},          "Pushes data or data from File to Server for uri could be /push?ep=<serial number>, "
                                                                                                                         "/set, /rd/<registration code>?lt=<seconds>, /bs?ep=<serial number>" },
  { "put",           {{"data"}, {"uri"}, {"file"}, {"content-format"}, {"uri-query"}, {"is-confirmable=true"}},          "Pushes data or data from File to Server for uri could be /push?ep=<serial number>, "
                                                                                                                         "/set, /rd/<registration code>?lt=<seconds>, /bs?ep=<serial number>" },
  { "get",           {{"data"}, {"uri"}, {"file"}, {"content-format"}, {"uri-query"}, {"is-confirmable=true"}},          "Pushes data or data from File to Server for uri could be /push?ep=<serial number>, "
                                                                                                                         "/set, /rd/<registration code>?lt=<seconds>, /bs?ep=<serial number>" },
  { "delete",        {{"data"}, {"uri"}, {"file"}, {"content-format"}, {"uri-query"}, {"is-confirmable=true"}},          "Pushes data or data from File to Server for uri could be /push?ep=<serial number>, "
                                                                                                                         "/set, /rd/<registration code>?lt=<seconds>, /bs?ep=<serial number>" },
  
  { "quit",           {},                                                                                                "Exit from application" },

};

std::vector<Readline::command>::iterator Readline::commands_iter = Readline::commands.begin();
std::vector<std::string>::iterator Readline::command_arg_iter = Readline::selected_command.argv.begin();
Readline::command Readline::selected_command;

std::string Readline::cmdName(void) {
  return(m_cmdName);
}

void Readline::cmdName(std::string cmdName) {
    m_cmdName = cmdName;
}

/*
 * @brief 
 * @param 
 * @return
 */
void Readline::prompt(std::string p) {
  m_prompt = p;
}

/*
 * @brief 
 * @param 
 * @return
 */
std::string Readline::prompt() {
  return(m_prompt);
}

char *commandArgListGenerator(const char *text, int state) {
    /* If this is a new word to complete, initialize now.  This includes
       saving the length of TEXT for efficiency, and initializing the index
       variable to 0. */
    if (!state) {
        Readline::m_argOffset = 0;
    }
    
    int &inner = Readline::m_argOffset;
    std::cout << basename(__FILE__) << ":" << __LINE__ << " text: " << text << std::endl;
    if(Readline::selected_command.argv[inner].empty())
        return((char *)NULL);

    return(strdup(Readline::selected_command.argv[inner++].c_str()));
}


char *commandArgGenerator(const char *text, int state) {
    std::string inputText(text);
    /* If this is a new word to complete, initialize now.  This includes
       saving the length of TEXT for efficiency, and initializing the index
       variable to 0. */
    if (!state) {
        Readline::command_arg_iter = Readline::selected_command.argv.begin();
    }

    /* Return the next name which partially matches from the command list. */
    auto it = std::find_if(Readline::command_arg_iter, Readline::selected_command.argv.end(), [&](const auto& ent) -> bool {
      return(!(ent.compare(0, inputText.length(), inputText)));
    });

    if(it != Readline::selected_command.argv.end()) {
        std::string arg = *it;
        Readline::command_arg_iter = std::next(it);
        return(strdup(arg.c_str()));
    }

    /*Reset to default either for next command or command argument(s).*/
    Readline::command_arg_iter = Readline::selected_command.argv.begin();

    /* If no names matched, then return NULL. */
    return((char *)NULL);
}

/* Generator function for command completion.  STATE lets us know whether
 * to start from scratch; without any state (i.e. STATE == 0), then we
 * start at the top of the list.
 * Note: This Function is kept invoking by readline until it returns NULL.
 */
char *commandGenerator(const char *text, int state) {
    std::string inputText(text);
    /* If this is a new word to complete, initialize now.  This includes
       saving the length of TEXT for efficiency, and initializing the index
       variable to 0. */
    if (!state) {
        Readline::commands_iter = Readline::commands.begin();
    }

    /* Return the next name which partially matches from the command list. */
    auto it = std::find_if(Readline::commands_iter, Readline::commands.end(), [&](const auto& ent) -> bool {
        return(!(ent.command.compare(0, inputText.length(), inputText)));
    });

    if(it != Readline::commands.end()) {
        Readline::selected_command = *it;
        auto &ent = *it;
        Readline::commands_iter = std::next(it);
        return(strdup(ent.command.c_str()));
    }

    /*Reset to default either for next command or command argument(s).*/
    Readline::commands_iter = Readline::commands.begin();

    /* If no names matched, then return NULL. */
    return((char *)NULL);
}

/* Attempt to complete on the contents of TEXT.  START and END show the
 * region of TEXT that contains the word to complete.  We can use the
 * entire line in case we want to do some simple parsing.  Return the
 * array of matches, or NULL if there aren't any. 
 */
char **commandCompletion(const char *text, int start, int end) {
    char **matches;
    matches = (char **)NULL;

    /* If this word is at the start of the line, then it is a command
       to complete.  Otherwise it is the name of a file in the current
       directory. */
    if(start == 0) {
        matches = rl_completion_matches(text, commandGenerator);
    } else {
        /*user has hit the space bar after command*/
        if(start == end) {
            /*remember it into its context - this is the command whose argument(s) to be listed.*/
            Readline::cmdName(std::string(rl_line_buffer));
            auto it = std::find_if(Readline::commands.begin(), Readline::commands.end(), [&](const auto& ent) -> bool {
                return(!ent.command.compare(0, Readline::cmdName().length(), Readline::cmdName()));
            });

            if(it != Readline::commands.end()) {
                matches = rl_completion_matches(text, commandArgListGenerator);    
            } else {
                /*user has entered the initials of argument*/
                matches = rl_completion_matches(text, commandArgGenerator);
            }
        } else {
            /*user has entered the initials of argument*/
            matches = rl_completion_matches(text, commandArgGenerator);
        }
    }
    return(matches);
}

int Readline::init(void) {
    /* rl_attempted_completion_over variable to a non-zero value, 
       Readline will not perform its default completion even if this function returns no matches*/
    rl_attempted_completion_over = 1; 
    /* Tell the completer that we want a crack first. */
    rl_attempted_completion_function = commandCompletion;
    return(0);
}

Readline::~Readline() {
  
}

Readline::Readline(std::shared_ptr<App> app) {
  m_continueStatus = false;
  m_app = app;
}

/* Look up NAME as the name of a command, and return a pointer to that
   command.  Return a NULL pointer if NAME isn't a command name. */

bool Readline::isValid(const std::string& cmd) {
    return(!cmd.compare(0, cmdName().length(), cmdName()));
}

/* Strip whitespace from the start and end of STRING.  Return a pointer
   into STRING. */
std::string Readline::ltrim(const std::string &s) {
    size_t start = s.find_first_not_of("\t ");
    return (start == std::string::npos) ? "" : s.substr(start);
}
 
std::string Readline::rtrim(const std::string &s) {
    size_t end = s.find_last_not_of("\t ");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}
 
std::string Readline::trim(const std::string &s) {
    return rtrim(ltrim(s));
}

bool Readline::start(std::string PS) {

    prompt(PS);
    /* Loop reading and executing lines until the user quits. */
    for( ; !continueStatus(); ) {
        std::unique_ptr<char> line(readline(prompt().c_str()));

        if(!line)
            break;
        
        std::string inputText(line.get());
        line.reset(nullptr);
        /* Remove leading and trailing whitespace from the line.
           Then, if there is anything left, add it to the history list
           and execute it. */
        inputText = trim(inputText);
        
        if(!inputText.empty()) {
            add_history(inputText.c_str());
            if(!(executeLine(inputText))) {
                /*Send this command to hostapd via control interface.*/
                if(/*-1 == hostapdCtrlIF()->transmit(s)*/0) {
                    //ACE_ERROR((LM_ERROR, "Send to Hostapd Failed\n"));
                }
            }
        }
    }

    exit(0);
}

int Readline::executeLine(std::string command) {
    
    bool isFound = false;
    int status = 1;

    isFound = isValid(command);
    
    do  {

        if(!isFound) {
            help(command);
            /*bypass the following statement.*/
            break;
        }

        if(!command.compare(0, 4, "help") || 
           !command.compare(0, 1, "?")) {
            help(command);
            break;
        }

        if(!command.compare(0, 4, "quit")) {
            quit();
            break;
        }

        processCommand(command);
        std::cout << basename(__FILE__) << ":" << __LINE__ << " The Command is ---> " << command << std::endl;
        status = 0;

    } while(0);

    return(status);
}

/* Print out help for cmd, or for all of the commands if cmd is
 * not present. 
 */
void Readline::help(const std::string& inputCommand) {

    auto it = std::find_if(Readline::commands.begin(), Readline::commands.end(), [&](const auto& ent) -> bool {
        return(inputCommand == ent.command);
    });

    if(it != Readline::commands.end()) {
        auto ent = *it;
        std::cout << ent.command << "\t\t" << ent.commandUsages << std::endl;
    } else {
        ///no command is found, list all supported command.
        for(const auto&command: Readline::commands) {
            std::cout << command.command << "\t\t" << command.commandUsages << std::endl;
        }
    }
}

void Readline::quit(void) {
    continueStatus(true);
}

void Readline::continueStatus(bool status) {
    m_continueStatus = status;
}

bool Readline::continueStatus(void) {
    return(m_continueStatus);
}

int Readline::processCommand(const std::string& command) {
    std::istringstream istrstr;
    std::string cmd;
    CoAPAdapter coapAdapter;

    istrstr.rdbuf()->pubsetbuf(const_cast<char *>(command.data()), command.length());

    if(!istrstr.eof()) {
        istrstr >> cmd;
        ///Is this command valid?
        auto it = std::find_if(Readline::commands.begin(), Readline::commands.end(), [&](const auto& elem) -> bool {
            return(!elem.command.compare(cmd));
        });

        if(it == Readline::commands.end()) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " Error Invalid Command: " << cmd << std::endl;
            return(-1);
        }
    }

    std::vector<std::string> command_opts;
    /// Command is valid - proceede 
    while(!istrstr.eof()) {
        std::string ent;
        istrstr >> ent;
        command_opts.push_back(ent);
    }

    std::unordered_map<std::string, std::string> keyValueMap;
    std::ostringstream name, value;
    if(!command_opts.empty()) {
        ///Process Command Arguments
        for(const auto& opt: command_opts) {
            std::istringstream iss(opt);
            name.str("");
            value.str("");

            if(!iss.get(*name.rdbuf(), '=').eof()) {
                name.str().resize(iss.gcount());

                ///Is this a valid argument of a given command?
                auto it = std::find_if(Readline::commands.begin(),  Readline::commands.end(), [&](const auto& ent) -> bool {
                    return(!ent.command.compare(cmd));
                });

                if(it == Readline::commands.end()) {
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " Error Invalid Argument:" << name.str()  << 
                                                       " length:" << name.str().length() << std::endl;
                    return(-2);
                }

                ///get rid of '=' delimiter
                iss.get();
                if(iss.get(*value.rdbuf()).eof() && iss.gcount() < 0) {
                    ///empty value is provided for argument
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " Error Empty argument value is not allowed for argument:" 
                              << name.str() << std::endl;
                    return(-3);
                }
            }

            keyValueMap[name.str()] = value.str();
        }
        
        ///Validation passes
        std::vector<std::string> uris;
        if(!keyValueMap["uri"].empty()) {
            uris = str2Vector(keyValueMap["uri"], '/');
        }

        std::vector<std::string> queries;
        if(!keyValueMap["uri-query"].empty()) {
            queries = str2Vector(keyValueMap["uri-query"], '&');
        }
        
        std::uint16_t cf = 0;
        if(!keyValueMap["content-format"].empty()) {
            cf = std::stoi(keyValueMap["content-format"]);
        }

        std::vector<std::string> cbor;
        cbor.push_back("[{\"key\": \"v1\"}]");
        if(!keyValueMap["data"].empty() && keyValueMap["data"].length() <= 1024) {
            std::string content;
            cbor.clear();
            cborAdapter().json2cbor(keyValueMap["data"], content);
            cbor.push_back(content);
        }
        
        if(!keyValueMap["file"].empty()) {
            ///Read data from File
            std::string content;
            cbor.clear();
            content = cborAdapter().getJson(keyValueMap["file"]);
            coapAdapter.buildRequest(content, cbor); 
        }

        ///Method type...
        std::uint16_t method = 4;
        if(!cmd.compare(0, 3, "get")) {
            method = 1;
        } else if(!cmd.compare(0, 4, "post")) {
            method = 2;
        } else if(!cmd.compare(0, 3, "put")) {
            method = 3;
        }

        std::vector<std::string> res;
        if(coapAdapter.serialise(uris, queries, cbor, cf, method, res) && !res.empty()) {
            ///Serialize CoAP request.
            for(auto& ent: res) {
                for(auto const& elm: ent) {
                    printf("%0.2X ", (unsigned char)elm);
                }

                printf("\n");
                ///sending CoAP message to Peer now.
                auto it = std::find_if(app()->udpAdapter()->services().begin(), app()->udpAdapter()->services().end(), [&](auto& ent) -> bool {
                    return(UDPAdapter::ServiceType_t::LwM2MClient == ent.second->service());
                });

                if(it != app()->udpAdapter()->services().end()) {
                    auto& elm = *it;
                    auto len = app()->udpAdapter()->tx(ent, elm.second->service());
                    if(len < 0)
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error unable to sent topeer" << " strerror:" << std::strerror(errno)<< std::endl;
                    else 
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " Successfully sent" << std::endl;
                }
                
            }
        }
    }

    return(0);
}

std::vector<std::string> Readline::str2Vector(const std::string& in, char delim) {
    std::vector<std::string> res;
    if(in.empty()) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error input is empty" << std::endl;
        return(res);
    }

    std::istringstream iss(in);
    std::ostringstream value;
    while(!iss.get(*value.rdbuf(), delim).eof()) {

        if(value.str().empty()) {
            iss.get(); ///get rid of delimeter character now.
        } else {
            if(value.str().at(0) == '"' && value.str().substr(1).length() > 0) {
                res.push_back(value.str().substr(1));
            } else if(value.str().at(0) != '"') {
                res.push_back(value.str());
            }
            value.str("");
            iss.get(); ///get rid of delimeter character now.
        }
        
    }

    if(!value.str().empty()) {
        if(value.str().at(0) == '"') {
            value.str(value.str().substr(1));
        }

        std::istringstream iss(value.str());
        std::ostringstream key;
        iss.get(*key.rdbuf(), '"');
        res.push_back(key.str());
    }

    return(res);
}

int Readline::processResponse(char *rsp, int len) {
  return(0);
}

#endif /*__readline_cpp__*/
