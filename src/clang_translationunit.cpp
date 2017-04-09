/*
 * Copyright (c) 2016, Joe Roback <joe.roback@gmail.com>. All Rights Reserved.
 */
#include <cassert>
#include <cstdint>
#include <sstream>
#include <vector>
#include <cstdio>
#include <fstream>

#include <libgen.h>

#include "clang_helpers.hpp"
#include "command_line_args.hpp"
#include "completion.hpp"
#include "diagnostic.hpp"
#include "logger.hpp"
#include "unsaved_files.hpp"

#include "clang_translationunit.hpp"


namespace node_clang
{

/**
 * Asynchronous worker to parse or reparse translation unit
 */
class parse_worker
    : public Nan::AsyncWorker
{
public:
    explicit parse_worker(clang_translationunit* tu,
                          bool force_parse,
                          const v8::Local<v8::Array>& unsaved_files,
                          const v8::Local<v8::Object>& self,
                          const v8::Local<v8::Promise::Resolver>& resolver)
        : Nan::AsyncWorker(nullptr)
        , _tu(tu)
        , _force_parse(force_parse)
        , _unsaved_files(unsaved_files)
        , _ret(0)
    {
        SaveToPersistent("self", self);
        SaveToPersistent("resolver", resolver);
    }

    virtual void Execute() override
    {
        _ret = _tu->parse(_force_parse, _unsaved_files, _diagnostics);
        // optimization since these are no longer needed, free on uv thread...
        _unsaved_files.dispose();
    }

    virtual void WorkComplete() override
    {
        Nan::HandleScope scope;
        auto resolver = GetFromPersistent("resolver").As<v8::Promise::Resolver>();

        if (_ret != 0)
        {
            std::ostringstream os;
            os << "Error Parsing: " << _ret << std::endl;
            os << _tu->filename();
            resolver->Reject(Nan::GetCurrentContext(), Nan::Error(os.str().c_str()));
            return;
        }

        if (_diagnostics.empty())
        {
            resolver->Resolve(Nan::GetCurrentContext(), Nan::New<v8::Array>());
        }
        else
        {
            auto diagnostics = Nan::New<v8::Array>(_diagnostics.size());
            auto type_text = Nan::New<v8::String>("type").ToLocalChecked();
            auto text_text = Nan::New<v8::String>("text").ToLocalChecked();
            auto filepath_text = Nan::New<v8::String>("filePath").ToLocalChecked();
            auto range_text = Nan::New<v8::String>("range").ToLocalChecked();

            for (std::size_t i = 0; i < _diagnostics.size(); ++i)
            {
                if (_diagnostics[i].severity() == CXDiagnostic_Ignored)
                {
                    continue;
                }

                auto diagnostic = Nan::New<v8::Object>();
                diagnostic->Set(type_text, Nan::New<v8::String>(to_string(_diagnostics[i].severity())).ToLocalChecked());
                diagnostic->Set(text_text, Nan::New<v8::String>(_diagnostics[i].text()).ToLocalChecked());
                diagnostic->Set(filepath_text, Nan::New<v8::String>(_diagnostics[i].filename()).ToLocalChecked());

                auto range = Nan::New<v8::Array>(2);
                auto diag_range = _diagnostics[i].range();

                auto range_start = Nan::New<v8::Array>(2);
                range_start->Set(0, Nan::New<v8::Number>(diag_range.first.first));
                range_start->Set(1, Nan::New<v8::Number>(diag_range.first.second));
                range->Set(0, range_start);

                auto range_end = Nan::New<v8::Array>(2);
                range_end->Set(0, Nan::New<v8::Number>(diag_range.second.first));
                range_end->Set(1, Nan::New<v8::Number>(diag_range.second.second));
                range->Set(1, range_end);

                diagnostic->Set(range_text, range);
                diagnostics->Set(i, diagnostic);
            }

            resolver->Resolve(Nan::GetCurrentContext(), diagnostics);
        }
    }

private:
    clang_translationunit* _tu;
    bool _force_parse;
    unsaved_files _unsaved_files;
    int _ret;
    std::vector<diagnostic> _diagnostics;
};

/**
 * Asynchronous worker to provide completions for a given translation unit, line, column
 */
class completion_worker
    : public Nan::AsyncWorker
{
public:
    explicit completion_worker(clang_translationunit* tu,
                               const char* src_filename,
                               unsigned line,
                               unsigned column,
                               const v8::Local<v8::Array>& unsaved_files,
                               bool deprecated,
                               const v8::Local<v8::Object>& self,
                               const v8::Local<v8::String>& local_prefix,
                               const v8::Local<v8::Promise::Resolver>& resolver)
        : Nan::AsyncWorker(nullptr)
        , _tu(tu)
        , _src_filename(src_filename)
        , _line(line)
        , _column(column)
        , _unsaved_files(unsaved_files)
        , _deprecated(deprecated)
    {
        SaveToPersistent("self", self);
        SaveToPersistent("local_prefix", local_prefix);
        SaveToPersistent("resolver", resolver);
    }

    virtual void Execute() override
    {
        _completions = _tu->completions(_src_filename.c_str(), _line, _column, _unsaved_files, _deprecated);
        // optimization since these are no longer needed, free on uv thread...
        _unsaved_files.dispose();
    }

    virtual void WorkComplete() override
    {
        Nan::HandleScope scope;
        auto resolver = GetFromPersistent("resolver").As<v8::Promise::Resolver>();
        auto local_prefix = GetFromPersistent("local_prefix").As<v8::String>();

        if (_completions.empty())
        {
            resolver->Resolve(Nan::GetCurrentContext(), Nan::Null());
        }
        else
        {
            auto completions = Nan::New<v8::Array>(_completions.size());
            auto type_text = Nan::New<v8::String>("type").ToLocalChecked();
            auto text_text = Nan::New<v8::String>("text").ToLocalChecked();
            auto replace_text = Nan::New<v8::String>("replacementPrefix").ToLocalChecked();
            auto left_text = Nan::New<v8::String>("leftLabel").ToLocalChecked();
            auto display_text = Nan::New<v8::String>("displayText").ToLocalChecked();
            auto right_text = Nan::New<v8::String>("rightLabel").ToLocalChecked();
            auto desc_text = Nan::New<v8::String>("description").ToLocalChecked();

            for (std::size_t i = 0; i < _completions.size(); ++i)
            {
                auto completion = Nan::New<v8::Object>();
                completion->Set(type_text, Nan::New<v8::String>(_completions[i].type()).ToLocalChecked());
                completion->Set(text_text, Nan::New<v8::String>(_completions[i].text()).ToLocalChecked());
                completion->Set(replace_text, local_prefix);
                completion->Set(left_text, Nan::New<v8::String>(_completions[i].left_label()).ToLocalChecked());
                completion->Set(display_text, Nan::New<v8::String>(_completions[i].display_text()).ToLocalChecked());
                completion->Set(right_text, Nan::New<v8::String>(_completions[i].right_label()).ToLocalChecked());
                completion->Set(desc_text, Nan::New<v8::String>(_completions[i].description()).ToLocalChecked());
                completions->Set(i, completion);
            }

            resolver->Resolve(Nan::GetCurrentContext(), completions);
        }
    }

private:
    clang_translationunit* _tu;
    std::string _src_filename;
    unsigned _line;
    unsigned _column;
    unsaved_files _unsaved_files;
    bool _deprecated;
    std::vector<completion> _completions;
};

Nan::Persistent<v8::Function> clang_translationunit::constructor;

void clang_translationunit::initialize(v8::Local<v8::Object> exports)
{
    this_logger::log("clang_translationunit::initialize(%p)", &constructor);

    v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(allocate);
    tpl->SetClassName(Nan::New("ClangTranslationUnit").ToLocalChecked());
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    Nan::SetPrototypeMethod(tpl, "parse", parse);
    Nan::SetPrototypeMethod(tpl, "completions", completions);
    Nan::SetPrototypeMethod(tpl, "setArgs", set_args);
	Nan::SetPrototypeMethod(tpl, "setClangCompleteRoot", set_clang_complete_root);
    Nan::SetPrototypeMethod(tpl, "dispose", dispose);

    constructor.Reset(Nan::GetFunction(tpl).ToLocalChecked());
    Nan::Set(exports, Nan::New("ClangTranslationUnit").ToLocalChecked(), Nan::GetFunction(tpl).ToLocalChecked());
}

void clang_translationunit::allocate(const Nan::FunctionCallbackInfo<v8::Value>& info)
{
    // TODO: add some argument error checking?
    if (info.IsConstructCall())
    {
        clang_translationunit* obj = nullptr;

        if (info.Length() == 1 && info[0]->IsExternal())
        {
            v8::Local<v8::External> external = v8::Local<v8::External>::Cast(info[0]);
            obj = static_cast<clang_translationunit*>(external->Value());
        }
        else
        {
            v8::String::Utf8Value arg1(info[0]->ToString());
            std::string filename = *arg1;
            obj = new clang_translationunit(filename);
        }
        obj->Wrap(info.This());
        info.GetReturnValue().Set(info.This());
    }
    else
    {
        v8::Local<v8::Function> cons = Nan::New(constructor);
        v8::Local<v8::Value> argv[] = { info[0] };
        info.GetReturnValue().Set(Nan::NewInstance(cons, 1, argv).ToLocalChecked());
    }
}

void clang_translationunit::parse(const Nan::FunctionCallbackInfo<v8::Value>& info)
{
    clang_translationunit* obj = Nan::ObjectWrap::Unwrap<clang_translationunit>(info.Holder());
    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(Nan::GetCurrentContext()).ToLocalChecked();

    auto force_parse = Nan::To<bool>(info[0]).FromJust();
    auto unsaved_files = v8::Local<v8::Array>::Cast(info[1]);

    // perform parsing asynchronously on libuv thread
    auto worker = new parse_worker(obj, force_parse, unsaved_files, info.This(), resolver);
    Nan::AsyncQueueWorker(worker);

    // return promise
    info.GetReturnValue().Set(resolver->GetPromise());
}

void clang_translationunit::completions(const Nan::FunctionCallbackInfo<v8::Value>& info)
{
    clang_translationunit* obj = Nan::ObjectWrap::Unwrap<clang_translationunit>(info.Holder());
    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(Nan::GetCurrentContext()).ToLocalChecked();

    auto deprecated = Nan::To<bool>(info[0]).FromJust();
    auto local_prefix = Nan::To<v8::String>(info[1]).ToLocalChecked();
    auto local_filename = Nan::To<v8::String>(info[2]).ToLocalChecked();
    v8::String::Utf8Value src_filename(local_filename);
    auto line = Nan::To<unsigned>(info[3]).FromJust();
    auto column = Nan::To<unsigned>(info[4]).FromJust();
    auto unsaved_files = v8::Local<v8::Array>::Cast(info[5]);

    // perform completion lookup asynchronously on libuv thread
    auto worker = new completion_worker(obj,
                                        *src_filename,
                                        line + 1,
                                        column + 1,
                                        unsaved_files,
                                        deprecated,
                                        info.This(),
                                        local_prefix,
                                        resolver);
    Nan::AsyncQueueWorker(worker);

    // return promise
    info.GetReturnValue().Set(resolver->GetPromise());
}

void clang_translationunit::set_args(const Nan::FunctionCallbackInfo<v8::Value>& info)
{
    clang_translationunit* obj = Nan::ObjectWrap::Unwrap<clang_translationunit>(info.Holder());
    auto args = v8::Local<v8::Array>::Cast(info[0]);
    obj->set_args(args);
}

void clang_translationunit::set_clang_complete_root(const Nan::FunctionCallbackInfo<v8::Value>& info)
{
    clang_translationunit* obj = Nan::ObjectWrap::Unwrap<clang_translationunit>(info.Holder());
    auto args = v8::Local<v8::String>::Cast(info[0]);
    obj->set_clang_complete_root(args);
}

void clang_translationunit::dispose(const Nan::FunctionCallbackInfo<v8::Value>& info)
{
    clang_translationunit* obj = Nan::ObjectWrap::Unwrap<clang_translationunit>(info.Holder());
    obj->dispose();
}

clang_translationunit::clang_translationunit(const std::string& filename)
    : _filename(filename)
    , _index(clang_createIndex(0, 0))
    , _tunit(nullptr)
    , _args(new command_line_args())
{
    this_logger::log("clang_translationunit(%s)", _filename.c_str());
}

clang_translationunit::~clang_translationunit()
{
    this_logger::log("~clang_translationunit(%s)", _filename.c_str());

    dispose();
    clang_disposeIndex(_index);
    _index = nullptr;
}

int clang_translationunit::parse(bool force_parse,
                                 unsaved_files& unsaved_files,
                                 std::vector<diagnostic>& diagnostics)
{
    CXDiagnosticSet diagnostic_set = nullptr;
    int ret;

    {
        // clang operations on same TU are not thread-safe
        std::lock_guard<std::mutex> lock(_lock);

        if (!_tunit || force_parse)
        {
            /*
             * As of this writing, clang_defaultEditingTranslationUnitOptions() enables
             * CXTranslationUnit_PrecompiledPreamble and CXTranslationUnit_CacheCompletionResults.
             *
             * CXTranslationUnit_CacheCompletionResults requires the editor to reparse more often
             * as completions results can be outdated and incomplete otherwise.
             */
            unsigned options = clang_defaultEditingTranslationUnitOptions()
                             | CXTranslationUnit_CreatePreambleOnFirstParse
                             | CXTranslationUnit_DetailedPreprocessingRecord
                             | CXTranslationUnit_IncludeBriefCommentsInCodeCompletion
                             | CXTranslationUnit_Incomplete
                             #if CINDEX_VERSION_MINOR >= 34
                             | CXTranslationUnit_KeepGoing
                             #endif
                             ;

            // if we are forcing a full parse, make sure to dispose of the old one...
            dispose();

			std::vector<const char*> args = complete_args();

            ret = clang_parseTranslationUnit2(_index,
                                              _filename.c_str(),
                                              args.data(),
                                              args.size(),
                                              unsaved_files.data(),
                                              unsaved_files.size(),
                                              options,
                                              &_tunit);
        }
        else
        {
            ret = clang_reparseTranslationUnit(_tunit,
                                               unsaved_files.size(),
                                               unsaved_files.data(),
                                               clang_defaultReparseOptions(_tunit));
        }

        // bail on error
        if (ret != 0)
        {
            dispose();
            return ret;
        }

        // get complete set of diagnostics under lock
        diagnostic_set = clang_getDiagnosticSetFromTU(_tunit);
    }

    // process diagnostics outside of lock
    unsigned num_diagnostics = clang_getNumDiagnosticsInSet(diagnostic_set);
    diagnostics.reserve(num_diagnostics);
    assert(diagnostics.empty());

    for (unsigned i = 0; i < num_diagnostics; ++i)
    {
        diagnostics.emplace_back(clang_getDiagnosticInSet(diagnostic_set, i));
    }

    // dispose of diagnostics
    clang_disposeDiagnosticSet(diagnostic_set);

    return ret;
}

std::vector<completion> clang_translationunit::completions(const std::string& filename,
                                                           unsigned line,
                                                           unsigned column,
                                                           unsaved_files& unsaved_files,
                                                           bool deprecated)
{
     CXCodeCompleteResults* completions = nullptr;

     unsigned options = clang_defaultCodeCompleteOptions() |
                        CXCodeComplete_IncludeBriefComments |
                        CXCodeComplete_IncludeMacros;

     {
         std::lock_guard<std::mutex> lock(_lock);
         completions = clang_codeCompleteAt(_tunit,
                                            filename.c_str(),
                                            line,
                                            column,
                                            unsaved_files.data(),
                                            unsaved_files.size(),
                                            options);
    }

    std::vector<completion> completions2;
    completions2.reserve(completions->NumResults);

    for (unsigned i = 0; i < completions->NumResults; ++i)
    {
        CXCompletionResult& result = completions->Results[i];
        CXAvailabilityKind availability = clang_getCompletionAvailability(result.CompletionString);

        if ((availability == CXAvailability_Available) ||
            (deprecated && availability == CXAvailability_Deprecated))
        {
            completions2.emplace_back(result);
        }
    }

    clang_disposeCodeCompleteResults(completions);

    return completions2;
}

void clang_translationunit::set_args(v8::Local<v8::Array>& args)
{
    std::lock_guard<std::mutex> lock(_lock);
    _args.reset(new command_line_args(args));
}

void clang_translationunit::set_clang_complete_root(v8::Local<v8::String>& args)
{
    _clang_complete_args.clear();

	v8::String::Utf8Value rootv(args);
	std::string root = *rootv;

	char sdir[_filename.size() + 1];
	for(std::size_t i = 0; i < _filename.size(); i++)
		sdir[i] = _filename[i];

	sdir[_filename.size()] = '\0';

	char* dir = dirname(sdir);
	this_logger::log("clang_translationunit .clang_complete search root: %s search dir start: %s", (const char*)root.c_str(), (const char*)dir);

	while(std::string(dir).find(root) != std::string::npos)
	{
		this_logger::log("clang_translationunit .clang_complete searching in %s", dir);

		std::ifstream clang_complete(std::string(dir) + "/.clang_complete");
		if(clang_complete.is_open())
		{
			std::string line;

			while(std::getline(clang_complete, line))
			{
				this_logger::log("clang_translationunit .clang_complete arg %s", line.c_str());
			    _clang_complete_args.push_back(line);
			}
			
			_clang_complete_args.push_back("-working-directory=" + std::string(dir));
			this_logger::log("clang_translationunit .clang_complete -working-directory= %s", dir);

			break;
		}

		std::string previous = dir;
		dir = dirname(dir);
		if(previous == dir)
			break;
	}
}

void clang_translationunit::dispose()
{
    if (_tunit)
    {
        this_logger::log("clang_translationunit::dispose(%s) = %p", _filename.c_str(), _tunit);

        clang_disposeTranslationUnit(_tunit);
        _tunit = nullptr;
    }
}

std::vector<const char*> clang_translationunit::complete_args()
{
	std::vector<const char*> args;

	for(std::size_t i = 0; i < _args->size(); i++)
		args.push_back(_args->data()[i]);

	for(const std::string& arg : _clang_complete_args)
		args.push_back(arg.c_str());

	return args;
}

}
