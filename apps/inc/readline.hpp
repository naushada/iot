#ifndef __readline_hpp__
#define __readline_hpp__

#include <iostream>
#include <vector>
#include <algorithm>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "app.hpp"
#include "cbor_adapter.hpp"

extern "C" {
	#include <readline/readline.h>
	#include <readline/history.h>
}

class Readline
{
  public:
    struct command
    {
        std::string command;
        std::vector<std::string> argv;
        std::string commandUsages;  
    };

    static std::int32_t m_offset;
    static std::int32_t m_len;
    static std::string m_cmdName;
    static std::int32_t m_argOffset;
    static std::vector<Readline::command> commands;
    static Readline::command selected_command;
    static std::vector<std::string>::iterator command_arg_iter;
    static std::vector<Readline::command>::iterator commands_iter;

  public:
    Readline(std::shared_ptr<App> a);
    ~Readline();
    int init(void);

    friend char **commandCompletion(const char *text, int start, int end);
    friend char *commandGenerator(const char *text, int state);
    friend char *commandArgGenerator(const char *text, int state);
    friend char *commandArgListGenerator(const char *text, int state);

    void prompt(std::string command);
    std::string prompt(void);
    int executeLine(std::string line);
    int processCommand(const std::string& command);
    int processResponse(char *rsp, int len);
    std::string rtrim(const std::string &s);
    std::string ltrim(const std::string &s);
    std::string trim(const std::string &s);
    bool isValid(const std::string& cmd);
    void help(const std::string& cmd);
    void quit(void);
    bool continueStatus(void);
    void continueStatus(bool status);
    static void cmdName(std::string cmdName);
    static std::string cmdName(void);
    bool start(std::string prompt="LwM2MClient-->> ");
    std::shared_ptr<App>& get_app() {
      return(app);
    }

    CBORAdapter& get_cborAdapter() {
      return(cborAdapter);
    }

    std::vector<std::string> str2Vector(const std::string& in, char delim='/');
 
  private:
    std::string m_prompt;
    bool m_continueStatus;
    std::vector<command> m_commands;
    std::shared_ptr<App> app;
    CBORAdapter cborAdapter;
};

#endif /*__readline_hpp__*/
