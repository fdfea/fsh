#include <cstdlib>
#include <cstdio>

#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define EUSR        -5
#define MAXLINE     200

using namespace std;

static int parse_and_run_chain(const string &chain);
static int parse_and_run_pipeline(const string &line);
static int parse_and_run_command(const string &command, int in_fd, int out_fd);
static void print_err(const string &message, int status);

int main(void) 
{
    int status = 0;

    //get a command chain from the user, execute it, repeat
    string chain;
    cout << "> ";
    while (getline(cin, chain)) 
    {
        status = parse_and_run_chain(chain);
        cout << "> ";
    }
    
    return status;
}

static int parse_and_run_chain(const string &chain)
{
    int status = 0;

    //do nothing if command is too long
    if (chain.length() > MAXLINE) 
    {
        status = EUSR;
        print_err("Invalid command, too long", status);
        return status;
    }
    
    size_t first_and = chain.find("& ");
    if (first_and != string::npos && first_and < chain.find_first_not_of("& \t\n\r\f\v"))
    {
        status = EUSR;
        print_err("Invalid command, ampersand at beginning", status);
        return status;
    }

    size_t last_and = chain.rfind(" &");
    if (last_and != string::npos && last_and > chain.find_last_not_of("& \t\n\r\f\v"))
    {
        status = EUSR;
        print_err("Invalid command, ampersand at end", status);
        return status;
    }
    
    //separate the chain into individual pipelines
    vector<string> pipelines{};
    string pipeline;
    
    size_t pos = 0, last = 0;
    while ((pos = chain.find(" & ", last)) != string::npos)
    {
        pipeline = chain.substr(last, pos-last);
        //empty string between ampersands is invalid
        if (pipeline.find_first_not_of(" \t\n\r\f\v") == string::npos)
        {
            status = EUSR;
            print_err("Invalid command, invalid or empty ampersand", status);
            return status;
        }
        pipelines.push_back(pipeline);
        last = pos + 2;
    }
    pipeline = chain.substr(last);
    if (pipeline.find_first_not_of(" \t\n\r\f\v") == string::npos)
    {
        status = EUSR;
        print_err("Invalid command, invalid or empty ampersand", status);
        return status;
    }
    pipelines.push_back(pipeline);
    
    size_t i = 0;
    while (i < pipelines.size() && !(status = parse_and_run_pipeline(pipelines[i++])));
    
    return status;
}

static int parse_and_run_pipeline(const string &line)
{
    int status = 0;
    
    size_t first_pipe = line.find("| ");
    if (first_pipe != string::npos && first_pipe < line.find_first_not_of("| \t\n\r\f\v"))
    {
        status = EUSR;
        print_err("Invalid command, pipe at beginning", status);
        return status;
    }

    size_t last_pipe = line.rfind(" |");
    if (last_pipe != string::npos && last_pipe > line.find_last_not_of("| \t\n\r\f\v"))
    {
        status = EUSR;
        print_err("Invalid command, pipe at end", status);
        return status;
    }
    
    //separate the pipeline into individual commands
    vector<string> commands{};
    string command;
    
    size_t pos = 0, last = 0;
    while ((pos = line.find(" | ", last)) != string::npos)
    {
        command = line.substr(last, pos-last);
        //empty string and pipe is invalid
        if (command.find_first_not_of(" \t\n\r\f\v") == string::npos)
        {
            status = EUSR;
            print_err("Invalid command, invalid or empty pipe", status);
            return status;
        }
        commands.push_back(command);
        last = pos + 2;
    }
    command = line.substr(last);
    if (command.find_first_not_of(" \t\n\r\f\v") == string::npos)
    {
        status = EUSR;
        print_err("Invalid command, invalid or empty pipe", status);
        return status;
    }
    commands.push_back(command);
    
    int pipe_fds[2], in_fd = STDIN_FILENO, out_fd = STDOUT_FILENO;
    
    //execute each command in the pipeline sequentially
    size_t i;
    for (i = 0; i < commands.size() - 1; ++i)
    {
        //create a pipe to direct I/O across commands
        status = pipe(pipe_fds);
        if (status != 0)
        {
            status = errno;
            print_err("Creating pipe", status);
            return status;
        }
        
        out_fd = pipe_fds[1];
        
        status = parse_and_run_command(commands[i], in_fd, out_fd);
        if (status != 0)
        {
            print_err("Running command: \"" + commands[i] + "\"", status);
            close(out_fd);
            return status;
        }
        
        status = close(out_fd);
        if (status != 0)
        {
            status = errno;
            print_err("Closing write file in pipe", status);
            return status;
        }
        
        in_fd = pipe_fds[0];
    }
    
    status = parse_and_run_command(commands[i], in_fd, STDOUT_FILENO);
    if (status != 0)
    {
        print_err("Running command: \"" + commands[i] + "\"", status);
        if (in_fd != STDIN_FILENO) close(in_fd);
        return status;
    }
    
    if (in_fd != STDIN_FILENO)
    {
        status = close(in_fd);
        if (status != 0)
        {
            status = errno;
            print_err("Closing read file in pipe", status);
        }
    }
    
    return status;
}

static int parse_and_run_command(const string &command, int in_fd, int out_fd) 
{
    int status = 0;

    //do nothing if command is too long
    if (command.length() > MAXLINE) 
    {
        status = EUSR;
        print_err("Invalid command, too long", status);
        return status;
    }
     
    //tokenize command by whitespace, check for malformed commands
    vector<string> tokens{};
    istringstream iss{command};
    string token;

    bool hasInputRedir = false;
    bool hasOutputRedir = false;
    string inputRedirFile = "";
    string outputRedirFile = "";
    
    while (iss >> token) 
    {
        if (token == "<")
        {
            if (hasInputRedir || hasOutputRedir || inputRedirFile != "")
            {
                status = EUSR;
                print_err("Invalid command, invalid input redirection", status);
                return status;
            }
            hasInputRedir = true;
        }
        else if (token == ">")
        {
            if (hasOutputRedir || hasInputRedir || outputRedirFile != "")
            {
                status = EUSR;
                print_err("Invalid command, invalid output redirection", status);
                return status;
            }
            hasOutputRedir = true;
        }
        else
        {
            if (hasInputRedir)
            {
                inputRedirFile = token;
                hasInputRedir = false;
            }
            else if (hasOutputRedir)
            {
                outputRedirFile = token;
                hasOutputRedir = false;
            }
            else
            {
                tokens.push_back(token);
            }
        }
    }
    
    //last token cannot be redirection or pipe
    if (hasInputRedir || hasOutputRedir)
    {
        status = EUSR;
        print_err("Invalid command, ends with redirection operator", status);
        return status;
    }
    
    //do nothing if no arguments provided
    if (tokens.size() == 0) 
    {
        status = EUSR;
        print_err("Invalid command, none provided", status);
        return status;
    }
    
    //exit shell if first argument is "exit"
    if (tokens[0] == "exit") 
    {
        exit(EXIT_SUCCESS);
    }
    
    string prog = tokens[0];
    
    //create child process to execute command
    pid_t pid = fork();
    
    if (pid == 0) 
    {
        //redirect input to pipe
        if (in_fd != STDIN_FILENO)
        {
            status = dup2(in_fd, STDIN_FILENO);
            if (status < 0)
            {
                status = errno;
                print_err("Duplicating in file descriptor", status);
                exit(status);
            }
            
            status = close(in_fd);
            if (status != 0)
            {
                status = errno;
                print_err("Closing in file descriptor", status);
                exit(status);
            }
        }
        
        //redirect output to pipe
        if (out_fd != STDOUT_FILENO)
        {
            status = dup2(out_fd, STDOUT_FILENO);
            if (status <  0)
            {
                status = errno;
                print_err("Duplicating out file descriptor", status);
                exit(status);
            }
            
            status = close(out_fd);
            if (status != 0)
            {
                status = errno;
                print_err("Closing out file descriptor", status);
                exit(status);
            }
        }
    
        //do input redirection, as necessary
        if (inputRedirFile != "")
        {
            char *path = (char *) inputRedirFile.c_str();

            int fd = open(path, O_RDONLY);
            if (fd < 0)
            {
                status = errno;
                print_err("Opening file \"" + inputRedirFile + "\"", status);
                exit(status);
            }
            
            status = dup2(fd, STDIN_FILENO);
            if (status < 0)
            {
                status = errno;
                print_err("Duplicating file descriptor to \"" + inputRedirFile + "\"", status);
                exit(status);
            }
            
            status = close(fd);
            if (status != 0)
            {
                status = errno;
                print_err("Closing file \"" + inputRedirFile + "\"", status);
                exit(status);
            }
        }
        
        //do output redirection, as necessary
        if (outputRedirFile != "")
        {
            char *path = (char *) outputRedirFile.c_str();
            
            fflush(stdout);
            
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
            if (fd < 0)
            {
                status = errno;
                print_err("Opening file \"" + outputRedirFile + "\"", status);
                exit(status);
            }
            
            status = dup2(fd, STDOUT_FILENO);
            if (status < 0)
            {
                status = errno;
                print_err("Duplicating file descriptor to \"" + outputRedirFile + "\"", status);
                exit(status);
            }
            
            status = close(fd);
            if (status != 0)
            {
                status = errno;
                print_err("Closing file \"" + outputRedirFile + "\"", status);
                exit(status);
            }
        }
    
        //execute program with arguments
        char *path = (char *) prog.c_str();
        char **argv = new char *[tokens.size()+1];
        
        for (size_t i = 0; i < tokens.size(); ++i) 
        {
            argv[i] = (char *) tokens[i].c_str();
        }
        argv[tokens.size()] = NULL;
        
        (void) execvp(path, argv);
        status = errno;
        if (status == ENOENT)
        {
            print_err("Command not found \"" + prog + "\"", status);
        }
        else
        {
            print_err(prog + " exit status", status);
        }
        
        delete [] argv;
       
        exit(status);
    } 
    else if (pid > 0)
    {   
        //wait for child process to exit and print exit status
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
        {
            status = WEXITSTATUS(status);
            //cout << prog << " exit status: " << WEXITSTATUS(status) << endl;
        }
        else if (WIFSIGNALED(status))
        {
            status = WTERMSIG(status);
            //cout << prog << " exit status: " << WTERMSIG(status) << endl;
        }
        else
        {
            status = errno;
            print_err("Waiting for child process", status);
        }
    }
    else 
    {
        status = errno;
        print_err("Forking process", status);
    }
    
    return status;
}

//print an error with a descriptive message
static void print_err(const string &message, int status)
{
    cerr << "[ERROR] " << message << ": " << status << endl;
}

