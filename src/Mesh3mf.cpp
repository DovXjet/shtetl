#include "CompactMesh.h"

#include <QXmlStreamReader>

#include <zip.h>

#include <map>
#include <memory>
#include <stdexcept>

// A reader of the geometry of 3MF files (the 3D Manufacturing Format core
// specification): meshes, components and build items with their transforms and
// the unit. Colors, materials and the other extensions are ignored.

namespace
{
  // A 3MF transform: 3x4, row-vector convention, p' = p * M.
  struct Transform
  {
    double m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

    vsg::vec3 apply(const vsg::vec3& p) const
    {
      return vsg::vec3(float(m[0] * p.x + m[3] * p.y + m[6] * p.z + m[9]),
                       float(m[1] * p.x + m[4] * p.y + m[7] * p.z + m[10]),
                       float(m[2] * p.x + m[5] * p.y + m[8] * p.z + m[11]));
    }

    // The transform that does `first`, then `second`.
    static Transform compose(const Transform& first, const Transform& second)
    {
      Transform r;
      for (int row = 0; row < 4; ++row) // the 4th row of a 3x4 matrix is (0 0 0 1)
        for (int col = 0; col < 3; ++col)
        {
          double sum = row == 3 ? second.m[9 + col] : 0.0;
          for (int k = 0; k < 3; ++k)
          {
            const double a = row == 3 ? first.m[9 + k] : first.m[row * 3 + k];
            sum += a * second.m[k * 3 + col];
          }
          r.m[row * 3 + col] = sum;
        }
      return r;
    }

    static Transform fromAttribute(const QString& text)
    {
      Transform t;
      const QStringList parts = text.split(' ', Qt::SkipEmptyParts);
      if (parts.size() == 12)
        for (int i = 0; i < 12; ++i) t.m[i] = parts[i].toDouble();
      return t;
    }
  };

  struct Component
  {
    int objectId;
    Transform transform;
  };

  struct Object
  {
    std::vector<vsg::vec3> vertices;
    std::vector<vsg::uivec3> triangles;
    std::vector<Component> components;
  };

  double unitToMillimeters(const QString& unit)
  {
    if (unit == "micron") return 0.001;
    if (unit == "centimeter") return 10.0;
    if (unit == "inch") return 25.4;
    if (unit == "foot") return 304.8;
    if (unit == "meter") return 1000.0;
    return 1.0; // millimeter, the default
  }

  void instantiate(const std::map<int, Object>& objects, int id, const Transform& transform, IndexedMesh& out, int depth)
  {
    auto it = objects.find(id);
    if (it == objects.end() || depth > 32) return;
    const Object& object = it->second;

    const uint32_t base = static_cast<uint32_t>(out.vertices.size());
    for (const auto& v : object.vertices) out.vertices.push_back(transform.apply(v));
    for (const auto& t : object.triangles) out.triangles.emplace_back(t.x + base, t.y + base, t.z + base);
    for (const auto& c : object.components)
      instantiate(objects, c.objectId, Transform::compose(c.transform, transform), out, depth + 1);
  }
} // namespace

namespace
{
  // libzip, as in XjetStudio: unlike Qt's QZipReader it reads ZIP64 archives,
  // which Netfabb and XjetStudio write for every 3MF file.
  struct ZipCloser
  {
    void operator()(zip_t* zip) const { zip_discard(zip); }
  };
  using ZipArchive = std::unique_ptr<zip_t, ZipCloser>;

  QByteArray readEntry(zip_t* zip, zip_int64_t index)
  {
    zip_stat_t st;
    if (zip_stat_index(zip, static_cast<zip_uint64_t>(index), 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE)) return {};
    std::unique_ptr<zip_file_t, int (*)(zip_file_t*)> file(zip_fopen_index(zip, static_cast<zip_uint64_t>(index), 0), zip_fclose);
    if (!file) return {};
    QByteArray data(static_cast<qsizetype>(st.size), Qt::Uninitialized);
    zip_int64_t done = 0;
    while (done < static_cast<zip_int64_t>(st.size))
    {
      const zip_int64_t n = zip_fread(file.get(), data.data() + done, st.size - static_cast<zip_uint64_t>(done));
      if (n <= 0) return {};
      done += n;
    }
    return data;
  }
} // namespace

IndexedMesh read3MF(const QString& path)
{
  int zipError = 0;
  const QByteArray utf8Path = path.toUtf8();
  ZipArchive zip(zip_open(utf8Path.constData(), ZIP_RDONLY, &zipError));
  if (!zip) throw std::runtime_error("Cannot read " + path.toStdString() + " as a 3MF (zip) file");

  // The model part is "3D/3dmodel.model" in practice; otherwise take the first .model.
  zip_int64_t modelIndex = zip_name_locate(zip.get(), "3D/3dmodel.model", 0);
  if (modelIndex < 0)
    for (zip_int64_t i = 0, n = zip_get_num_entries(zip.get(), 0); i < n; ++i)
    {
      const char* name = zip_get_name(zip.get(), static_cast<zip_uint64_t>(i), 0);
      if (name && QString::fromUtf8(name).endsWith(".model", Qt::CaseInsensitive))
      {
        modelIndex = i;
        break;
      }
    }
  if (modelIndex < 0) throw std::runtime_error("No 3D model in " + path.toStdString());
  const QByteArray xml = readEntry(zip.get(), modelIndex);
  if (xml.isEmpty()) throw std::runtime_error("Cannot read the 3D model of " + path.toStdString());

  std::map<int, Object> objects;
  std::vector<Component> buildItems;
  double scale = 1.0;

  QXmlStreamReader reader(xml);
  Object* current = nullptr;
  bool inBuild = false;
  while (!reader.atEnd())
  {
    reader.readNext();
    if (reader.isStartElement())
    {
      const auto a = reader.attributes();
      const QStringView name = reader.name();
      if (name == u"model")
        scale = unitToMillimeters(a.value("unit").toString());
      else if (name == u"object")
        current = &objects[a.value("id").toInt()];
      else if (name == u"build")
        inBuild = true;
      else if (name == u"vertex" && current)
        current->vertices.emplace_back(a.value("x").toFloat(), a.value("y").toFloat(), a.value("z").toFloat());
      else if (name == u"triangle" && current)
        current->triangles.emplace_back(a.value("v1").toUInt(), a.value("v2").toUInt(), a.value("v3").toUInt());
      else if (name == u"component" && current)
        current->components.push_back({a.value("objectid").toInt(), Transform::fromAttribute(a.value("transform").toString())});
      else if (name == u"item" && inBuild)
        buildItems.push_back({a.value("objectid").toInt(), Transform::fromAttribute(a.value("transform").toString())});
    }
    else if (reader.isEndElement())
    {
      if (reader.name() == u"object") current = nullptr;
      if (reader.name() == u"build") inBuild = false;
    }
  }
  if (reader.hasError()) throw std::runtime_error("Bad 3MF model: " + reader.errorString().toStdString());

  // Indices that point outside their object's vertices would read out of range.
  for (auto& [id, object] : objects)
    for (const auto& t : object.triangles)
      if (t.x >= object.vertices.size() || t.y >= object.vertices.size() || t.z >= object.vertices.size())
        throw std::runtime_error("The 3MF has triangles with invalid vertex indices");

  IndexedMesh mesh;
  if (buildItems.empty()) // no build section: everything there is
    for (const auto& [id, object] : objects) buildItems.push_back({id, Transform()});
  for (const auto& item : buildItems) instantiate(objects, item.objectId, item.transform, mesh, 0);

  if (scale != 1.0)
    for (auto& v : mesh.vertices) v = v * float(scale);
  return mesh;
}
