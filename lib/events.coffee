#
# Copyright (c) 2016, Joe Roback <joe.roback@gmail.com>. All Rights Reserved.
#
fs = require 'fs'
os = require 'os'
path = require 'path'

debug = require './debug'
scope = require './scope'
util = require './util'

{CompositeDisposable} = require 'atom'
{ClangTranslationUnit} = require('bindings') 'clang.node'
{ParseCoalescer} = require './parsecoalescer'

disposables = null

# clang needs a valid file path, so if this is an empty buffer, create a unique temporary file path for the buffer
getEditorPath = (editor) ->
  filepath = editor.getPath()
  return filepath if filepath?
  filepath = path.resolve os.tmpdir(), "AtomClangBuffer-#{editor.getBuffer().getId()}"
  # TODO clean these up?
  fs.closeSync fs.openSync filepath, 'w' if not fs.existsSync filepath
  return filepath

# libclang args cannot have spaces in, from a user perspective that is annoying, correct it here
flags_re = /\s+/

formatFlags = (flags) ->
  formattedFlags = []
  flags.forEach (flag) ->
    formattedFlags.push.apply(formattedFlags, flag.split(flags_re))
  formattedFlags

# go through all the open buffers that are modified and collect their contents to give to libclang
buildUnsavedFiles = ->
  unsavedFiles = []
  buffers = new Set()
  atom.workspace.getTextEditors()
    .filter (editor) -> return not buffers.has(editor.getBuffer().id) and editor.isModified() and editor.clang?
    .forEach (editor) ->
      buffers.add editor.getBuffer().id
      unsavedFiles.push
        filename: getEditorPath editor
        contents: editor.getText()
  unsavedFiles

# reparse all translation units for every editor
reparseOtherEditors = (editor) ->
  atom.workspace.getTextEditors()
    .filter (other) -> return editor isnt other and other.clang?
    .forEach (other) -> other.clang.coalescer.reparse().catch util.showError []

# attempt to load a clang translation unit into this editor
load = (editor) ->
  return unless scope.isEnabledForScope editor.getRootScopeDescriptor()
  return unless fs.existsSync getEditorPath editor

  debug.log 'load', editor.clang, getEditorPath editor
  console.assert not editor.clang?, editor

  # perform a full translation unit parse
  parse = ->
    debug.log 'parse', editor.clang, buildUnsavedFiles(), getEditorPath editor
    return Promise.resolve([]) unless editor.clang?
    editor.clang.translationUnit.parse true, buildUnsavedFiles()

  # reparse translation unit
  reparse = ->
    debug.log 'reparse', editor.clang, buildUnsavedFiles(), getEditorPath editor
    return Promise.resolve([]) unless editor.clang?
    editor.clang.translationUnit.parse false, buildUnsavedFiles()

  editor.clang =
    translationUnit: null
    coalescer: null
    completions: null
    subscriptions: null

  # create native libclang translation unit object for parsing/completions
  editor.clang.translationUnit = new ClangTranslationUnit getEditorPath editor
  editor.clang.translationUnit.setClangCompleteRoot atom.workspace.project.getPaths()[0]

  # this queue ensures us that only one parse/reparse is ever outstanding
  editor.clang.coalescer = new ParseCoalescer parse, reparse

  editor.clang.completions = (deprecated, prefix, line, column) ->
    editor.clang.translationUnit.completions \
      deprecated,
      prefix,
      getEditorPath(editor),
      line,
      column,
      buildUnsavedFiles()

  # register for config subscriptions
  editor.clang.subscriptions = new CompositeDisposable()

  # if path changes, attempt to reload, as clang works on physical files
  editor.clang.subscriptions.add editor.onDidChangePath ->
    debug.log 'onDidChangePath', editor.clang, getEditorPath editor
    reload editor

  # if we save, we need to reparse all other clang buffers
  # clang_codeCompleteAt() will not reparse another translation unit
  # because another one was saved, even if that file was a header and
  # and was a dependency of the translation unit it is trying to provide
  # completions for. if files are modified outside Atom, undefined results
  # will happen, best case --- wrong completion data is returned.
  #
  # once the new linter indie API is available: https://github.com/steelbrain/linter/issues/1166
  # we can just push the linter messages from here...
  editor.clang.subscriptions.add editor.onDidSave ->
    debug.log 'onDidSave', editor.clang, getEditorPath editor
    reparseOtherEditors editor

  # clang translation unit uses CXTranslationUnit_CacheCompletionResults, so we
  # need to periodically reparse the translation unit to keep the results fresh
  # removing the CXTranslationUnit_CacheCompletionResults flag results in really,
  # really laggy completions...
  editor.clang.subscriptions.add editor.onDidStopChanging ->
    debug.log 'onDidStopChanging', editor.clang, getEditorPath editor
    editor.clang.coalescer.reparse().catch util.showError []

  # setup clang flags based on scope/grammar, listen for config changes
  # if the grammar changes, the editor observer will fully unload and re-load
  #
  # -Wno-pragma-once-outside-header is needed when trying to make a translation unit
  # on a header file. It seems like you can code completion by just having one translation unit
  # per header/source pair, which is more efficient, but its unclear how to properly
  # handle a header only implementation file... At this point having a translation unit
  # per editor, even if its duplication over split panes, is easier to manage.
  # after this has a lot of the bugs and features worked out, this optimization can be revisited...

  if scope.isCScope editor.getRootScopeDescriptor()
    editor.clang.subscriptions.add atom.config.observe 'atom-clang.defaultCFlags', (flags) ->
      flags.push '-xc'
      flags.push '-Wno-pragma-once-outside-header'
      editor.clang.translationUnit.setArgs formatFlags flags
      editor.clang.coalescer.parse().catch util.showError()
  else if scope.isCppScope editor.getRootScopeDescriptor()
    editor.clang.subscriptions.add atom.config.observe 'atom-clang.defaultCXXFlags', (flags) ->
      flags.push '-xc++'
      flags.push '-Wno-pragma-once-outside-header'
      editor.clang.translationUnit.setArgs formatFlags flags
      editor.clang.coalescer.parse().catch util.showError()
  else
    editorScope = scope.getScope editor.getRootScopeDescriptor()
    util.showError() new Error "Unable to match file type: #{editorScope}"

# delete and dispose all clang related properties of the editor
unload = (editor) ->
  return unless editor.clang?
  debug.log 'unload', editor.clang, getEditorPath editor
  editor.clang.subscriptions.dispose()
  editor.clang.translationUnit.dispose()
  delete editor.clang

# unload clang, attempt to load
reload = (editor) ->
  unload editor
  load editor

#
# the general idea is we start a clang translation unit per editor
# if the same underlying buffer is split between panes, there will be
# multiple translation units. this is because Atom allows the user to
# change scopes/grammars on a per editor/pane, so a header parsed with
# C grammar will have different arguments to clang then C++
#

observeEditors = ->
  atom.workspace.observeTextEditors (editor) ->
    #
    # register observers for editor
    #
    # the main idea here is to load the least amount possible, as we really
    # only want to pollute editor/buffer objects for grammars we support, i.e. C/C++
    #
    # these events are the bare minimum. when we see them we have to
    # unload and reload the translation unit, mainly because it means 1 of 2 things:
    #   1) we are seeing a supported grammar for the first time (C/C++)
    #   2) we are changing file paths on disk (translation unit is tied to file path in LibClang)
    #
    editorDisposables = new CompositeDisposable()

    editorDisposables.add editor.observeGrammar ->
      debug.log 'observeGrammar', editor.clang, getEditorPath editor
      reload editor

    editorDisposables.add editor.onDidSave ->
      debug.log 'onDidSave', editor.clang, getEditorPath editor
      load editor if not editor.clang?

    # remove all observers, dispose of atom-clang state
    editorDisposables.add editor.onDidDestroy ->
      debug.log 'onDidDestroy', editor.clang, getEditorPath editor
      editorDisposables.dispose()
      editorDisposables = null
      unload editor

activate = ->
  disposables = new CompositeDisposable()
  disposables.add observeEditors()

deactivate = ->
  disposables.dispose()
  disposables = null
  atom.workspace.getTextEditors().forEach (editor) -> unload editor

module.exports = {
  activate
  deactivate
}
