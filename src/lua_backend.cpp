/*
 * HDF5-UDF: User-Defined Functions for HDF5
 *
 * File: main.cpp
 *
 * Lua code parser and bytecode generation/execution.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "lua_backend.h"
#include "dataset.h"
#include "lua.hpp"

/* Lua context */
static lua_State *State;

#define DATA_OFFSET(i)        (void *) (((char *) &State) + i)
#define NAME_OFFSET(i)        (void *) (((char *) &State) + 100 + i)
#define DIMS_OFFSET(i)        (void *) (((char *) &State) + 200 + i)
#define TYPE_OFFSET(i)        (void *) (((char *) &State) + 300 + i)
#define CAST_OFFSET(i)        (void *) (((char *) &State) + 400 + i)

extern "C" int index_of(const char *element)
{
    for (int index=0; index<100; ++index) {
        /* Set register key to get datasets name vector */
        lua_pushlightuserdata(State, NAME_OFFSET(index));
        lua_gettable(State, LUA_REGISTRYINDEX);
        const char *name = lua_tostring(State, -1);
        if (! strcmp(name, element))
            return index;
        else if (strlen(name) == 0)
            break;
    }
    fprintf(stderr, "Error: dataset %s not found\n", element);
    return -1;
}

/* Functions exported to the Lua template library (udf.lua) */
extern "C" void *get_data(const char *element)
{
    int index = index_of(element);
    if (index >= 0)
    {
        /* Get datasets contents */
        lua_pushlightuserdata(State, DATA_OFFSET(index)); 
        lua_gettable(State, LUA_REGISTRYINDEX);
        return lua_touserdata(State, -1);
    }
    return NULL;
}

extern "C" const char *get_type(const char *element)
{
    int index = index_of(element);
    if (index >= 0)
    {
        /* Set register key to get dataset type */
        lua_pushlightuserdata(State, TYPE_OFFSET(index));
        lua_gettable(State, LUA_REGISTRYINDEX);
        return lua_tostring(State, -1);
    }
    return NULL;
}

extern "C" const char *get_cast(const char *element)
{
    int index = index_of(element);
    if (index >= 0)
    {
        /* Set register key to get dataset type */
        lua_pushlightuserdata(State, CAST_OFFSET(index));
        lua_gettable(State, LUA_REGISTRYINDEX);
        return lua_tostring(State, -1);
    }
    return NULL;
}

extern "C" const char *get_dims(const char *element)
{
    int index = index_of(element);
    if (index >= 0)
    {
        /* Set register key to get dataset size */
        lua_pushlightuserdata(State, DIMS_OFFSET(index));
        lua_gettable(State, LUA_REGISTRYINDEX);
        return lua_tostring(State, -1);
    }
    return NULL;
}

/* This backend's name */
std::string LuaBackend::name()
{
    return "LuaJIT";
}

/* Extension managed by this backend */
std::string LuaBackend::extension()
{
    return ".lua";
}

/* Compile Lua to bytecode using LuaJIT. Returns the bytecode as a string. */
std::string LuaBackend::compile(std::string udf_file, std::string template_file)
{
    std::string bytecode;
    std::ifstream ifs(udf_file);
    if (! ifs.is_open())
    {
        fprintf(stderr, "Failed to open %s\n", udf_file.c_str());
        return "";
    }
    std::string inputFileBuffer(
		(std::istreambuf_iterator<char>(ifs)),
        (std::istreambuf_iterator<char>()  ));

    /* Basic check: does the template file exist? */
    if (template_file.size() == 0)
    {
        fprintf(stderr, "Failed to find Lua template file\n");
        return "";
    }
    std::ifstream ifstr(template_file);
    std::string udf(
		(std::istreambuf_iterator<char>(ifstr)),
        (std::istreambuf_iterator<char>()    ));

    /* Basic check: is the template string present in the template file? */
    std::string placeholder = "-- user_callback_placeholder";
    auto start = udf.find(placeholder);
    if (start == std::string::npos)
    {
        fprintf(stderr, "Failed to find placeholder string in %s\n",
            template_file.c_str());
        return "";
    }

    /* Embed UDF string in the template */
    auto completeCode = udf.replace(start, placeholder.length(), inputFileBuffer);

    /* Compile the code */
    std::ofstream tmpfile;
    char buffer [32];
    sprintf(buffer, "hdf5-udf-XXXXXX");
    if (mkstemp(buffer) < 0){
        fprintf(stderr, "Error creating temporary file.\n");
        return std::string("");
    }
    tmpfile.open (buffer);
    tmpfile << completeCode.data();
    tmpfile.flush();
    tmpfile.close();

    std::string output = udf_file + ".bytecode";
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process
        char *cmd[] = {
            (char *) "luajit",
            (char *) "-O3",
            (char *) "-b",
            (char *) buffer,
            (char *) output.c_str(),
            (char *) NULL
        };
        execvp(cmd[0], cmd);
    }
    else if (pid > 0)
    {
        // Parent
        int exit_status;
        wait4(pid, &exit_status, 0, NULL);

        struct stat statbuf;
        if (stat(output.c_str(), &statbuf) == 0) {
            printf("Bytecode has %ld bytes\n", statbuf.st_size);

            std::ifstream data(output, std::ifstream::binary);
            std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(data), {});
            bytecode.assign(buffer.begin(), buffer.end());

            unlink(output.c_str());
        }
        unlink(buffer);
        return bytecode;
    }
    fprintf(stderr, "Failed to execute luajit\n");
    return bytecode;
}

/* Execute the user-defined-function embedded in the given bytecode */
bool LuaBackend::run(
    const std::string filterpath,
    const std::vector<DatasetInfo> input_datasets,
    const DatasetInfo output_dataset,
    const char *output_cast_datatype,
    const char *bytecode,
    size_t bytecode_size)
{
    lua_State *L = luaL_newstate();
    State = L;

    lua_pushcfunction(L, luaopen_base);
    lua_call(L,0,0);
    lua_pushcfunction(L, luaopen_math);
    lua_call(L,0,0);
    lua_pushcfunction(L, luaopen_string);
    lua_call(L,0,0);
    lua_pushcfunction(L, luaopen_ffi);
    lua_call(L,0,0);
    lua_pushcfunction(L, luaopen_jit);
    lua_call(L,0,0);
    lua_pushcfunction(L, luaopen_package);
    lua_call(L,0,0);
    lua_pushcfunction(L, luaopen_table);
    lua_call(L,0,0);

    DatasetInfo empty_entry;
    std::vector<DatasetInfo> dataset_info;
    dataset_info.push_back(output_dataset);
    dataset_info.insert(
        dataset_info.end(), input_datasets.begin(), input_datasets.end());
    dataset_info.push_back(empty_entry);

    /* Populate vector of dataset names, sizes, and types */
    for (size_t i=0; i<dataset_info.size(); ++i)
    {
        /* Grid */
        lua_pushlightuserdata(L, DATA_OFFSET(i));
        lua_pushlightuserdata(L, (void *) dataset_info[i].data);
        lua_settable(L, LUA_REGISTRYINDEX);

        /* Name */
        lua_pushlightuserdata(L, NAME_OFFSET(i));
        lua_pushstring(L, dataset_info[i].name.c_str());
        lua_settable(L, LUA_REGISTRYINDEX);

        /* Dimensions */
        lua_pushlightuserdata(L, DIMS_OFFSET(i));
        lua_pushstring(L, dataset_info[i].dimensions_str.c_str());
        lua_settable(L, LUA_REGISTRYINDEX);

        /* Type */
        lua_pushlightuserdata(L, TYPE_OFFSET(i));
        lua_pushstring(L, dataset_info[i].getDatatype());
        lua_settable(L, LUA_REGISTRYINDEX);

        /* Type, used for casting purposes */
        lua_pushlightuserdata(L, CAST_OFFSET(i));
        lua_pushstring(L, dataset_info[i].getCastDatatype());
        lua_settable(L, LUA_REGISTRYINDEX);
    }

    int retValue = luaL_loadbuffer(L, bytecode, bytecode_size, "hdf5_udf_bytecode");
    if (retValue != 0)
    {
        fprintf(stderr, "luaL_loadbuffer failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    if (lua_pcall(L, 0, 0 , 0) != 0)
    {
        fprintf(stderr, "Failed to load the bytecode: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return false;
    }

    // Initialize the UDF library
    lua_getglobal(L, "init");
    lua_pushstring(L, filterpath.c_str());
    if (lua_pcall(L, 1, 0, 0) != 0)
    {
        fprintf(stderr, "Failed to invoke the init callback: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return false;
    }

    // Call the UDF entry point
    lua_getglobal(L, "dynamic_dataset");
    if (lua_pcall(L, 0, 0, 0) != 0)
    {
        fprintf(stderr, "Failed to invoke the dynamic_dataset callback: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    lua_close(L);

    return true;   
}

/* Scan the UDF file for references to HDF5 dataset names */
std::vector<std::string> LuaBackend::udfDatasetNames(std::string udf_file)
{
    std::string input;
    std::ifstream data(udf_file, std::ifstream::binary);
    std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(data), {});
    input.assign(buffer.begin(), buffer.end());

    std::string line;
    bool is_comment = false;
    std::istringstream iss(input);
    std::vector<std::string> output;

    auto ltrim = [](std::string &s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
            return !std::isspace(ch);
        }));
    };

    while (std::getline(iss, line))
    {
        ltrim(line);
        if (line.find("--[=====[") != std::string::npos ||
            line.find("--[====[") != std::string::npos ||
            line.find("--[===[") != std::string::npos ||
            line.find("--[==[") != std::string::npos ||
            line.find("--[=[") != std::string::npos ||
            line.find("--[[") != std::string::npos)
            is_comment = true;
        else if (is_comment && (
            line.find("]=====]") != std::string::npos ||
            line.find("]====]") != std::string::npos ||
            line.find("]===]") != std::string::npos ||
            line.find("]==]") != std::string::npos ||
            line.find("]=]") != std::string::npos ||
            line.find("]]") != std::string::npos))
            is_comment = false;
        else if (! is_comment)
        {
            auto n = line.find("lib.getData");
            auto c = line.find("--");
            if (n != std::string::npos && (c == std::string::npos || c > n))
            {
                auto start = line.substr(n).find_first_of("\"");
                auto end = line.substr(n+start+1).find_first_of("\"");
                auto name = line.substr(n).substr(start+1, end);
                output.push_back(name);
            }
        }
    }
    return output;
}