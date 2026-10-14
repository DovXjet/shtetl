#pragma once

#include <QFile>

#include <stdexcept>
#include <string>

// The text of a shader in the Qt resources (":/shaders/...").
inline std::string shaderResourceText(const char* path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(std::string("Missing resource ") + path);
  return file.readAll().toStdString();
}

// Pastes ":/shaders/<name>.glsl" in place of each line "#pragma include <name>".
inline std::string expandShaderIncludes(std::string source)
{
  const std::string marker = "#pragma include ";
  for (size_t at = source.find(marker); at != std::string::npos; at = source.find(marker))
  {
    const size_t nameStart = at + marker.size();
    size_t nameEnd = source.find_first_of("\r\n", nameStart);
    if (nameEnd == std::string::npos) nameEnd = source.size();
    const std::string name = source.substr(nameStart, nameEnd - nameStart);
    source.replace(at, nameEnd - at, shaderResourceText((":/shaders/" + name + ".glsl").c_str()));
  }
  return source;
}
