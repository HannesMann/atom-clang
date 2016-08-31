#
# Copyright (c) 2016, Joe Roback <joe.roback@gmail.com>. All Rights Reserved.
#
module.exports = {
  linterEnabled:
    title: 'Enable Linter'
    description: 'Disable linter if you do not want clang diagnostic messages.'
    type: 'boolean'
    default: true
    order: 1
  includeDeprecated:
    title: 'Include deprecated items in code completion results.'
    description: 'Disable if you prefer not to see items in code completion marked as deprecated.'
    type: 'boolean'
    default: true
    order: 2
  maxCompletions:
    title: 'The maximum number of completions to return from clang for single completion attempt.'
    description: 'Set to 0 for all results. ~500 is probably a reasonable maximum for most people.'
    type: 'integer'
    default: 500
    minimum: 0
    order: 3
  defaultCFlags:
    title: 'Default CFlags'
    description: 'CFLAGS to pass to clang parser. Comma separated.'
    type: 'array'
    default: [
      '-std=c99'
      '-Wall'
    ]
    items:
      type: 'string'
    order: 4
  defaultCXXFlags:
    title: 'Default CXXFlags'
    description: 'CXXFLAGS to pass to clang parser. Comma separated.'
    type: 'array'
    default: [
      '-std=c++11'
      '-Wall'
      '-fexceptions'
    ]
    items:
      type: 'string'
    order: 5
}
