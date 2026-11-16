#include "CadReader.h"

#include <IGESControl_Reader.hxx>
#include <IMeshTools_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>
#include <OSD_ThreadPool.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <QFileInfo>
#include <QSettings>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
  constexpr const char* AbortMessage = "Abort requested!";

  // OpenCASCADE's progress and its user break, bridged to ours: the part of
  // 0..100 from `from` to `to` is what this indicator reports.
  class ProgressBridge : public Message_ProgressIndicator
  {
    public:
      ProgressBridge(const std::atomic<bool>* cancel, const LoadProgress& progress, double from, double to)
          : m_Cancel(cancel), m_Progress(progress), m_From(from), m_To(to)
      {
      }

      bool UserBreak() override { return m_Cancel && m_Cancel->load(); }

      void Show(const Message_ProgressScope&, const Standard_Boolean) override
      {
        if (m_Progress) m_Progress(static_cast<int>(m_From + (m_To - m_From) * GetPosition()));
      }

    private:
      const std::atomic<bool>* m_Cancel;
      LoadProgress m_Progress;
      double m_From, m_To;
  };

  void throwIfCancelled(const std::atomic<bool>* cancel)
  {
    if (cancel && cancel->load()) throw std::runtime_error(AbortMessage);
  }

  // OCCT runs its parallel loops on a default pool; size it once, before any
  // read or mesh (a thumbnail and the viewer may read at the same time).
  void initThreadPool()
  {
    static std::once_flag once;
    std::call_once(once, []() { OSD_ThreadPool::DefaultPool()->Init(static_cast<int>(std::max(1u, std::thread::hardware_concurrency()))); });
  }

  // Whether there is anything to mesh. Faces are what get meshed, so a file of
  // loose surfaces works as well as one of solids.
  bool hasShapes(const TopoDS_Shape& shape)
  {
    return !shape.IsNull() && TopExp_Explorer(shape, TopAbs_FACE).More();
  }

  template <class Reader>
  TopoDS_Shape readShape(Reader& reader, const char* what, const char* path, const std::atomic<bool>* cancel, const LoadProgress& progress)
  {
    if (reader.ReadFile(path) != IFSelect_RetDone) throw std::runtime_error(std::string("Failed to read the ") + what + " file");
    throwIfCancelled(cancel);

    // The read has no progress of its own: the transfer is the first fifth.
    if (progress) progress(5);
    Handle(Message_ProgressIndicator) indicator = new ProgressBridge(cancel, progress, 5, 20);
    reader.TransferRoots(indicator->Start());
    throwIfCancelled(cancel);

    TopoDS_Shape shape = reader.OneShape();
    // The transfer can succeed without any geometry (a file of wireframe or
    // unsupported entities only).
    if (!hasShapes(shape)) throw std::runtime_error(std::string("No transferable geometry found in the ") + what + " file");
    return shape;
  }

  // The assembly tree of a STEP file (see walkLabel): the nodes, and for the
  // leaves the located shape that makes up the part.
  struct StepTree
  {
      std::vector<PartNode> nodes;
      std::vector<TopoDS_Shape> shapes; // parallel to nodes; null for the groups
  };

  QString labelName(const TDF_Label& label)
  {
    Handle(TDataStd_Name) name;
    if (label.IsNull() || !label.FindAttribute(TDataStd_Name::GetID(), name)) return {};
    const TCollection_ExtendedString& text = name->Get();
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(text.ToExtString()), static_cast<qsizetype>(text.Length())).trimmed();
  }

  // Adds the shape of `label` to the tree: an assembly as a group of its
  // components (each placed by its own location, relative to the assembly), any
  // other shape as a leaf.
  void walkLabel(const TDF_Label& label, const TopLoc_Location& location, int parent, StepTree& tree, int depth)
  {
    if (depth > 64) return;
    TDF_Label referred = label;
    if (XCAFDoc_ShapeTool::IsReference(label)) XCAFDoc_ShapeTool::GetReferredShape(label, referred);
    QString name = labelName(label);
    // (A component with no name of its own is named "=>[label]" by the reader.)
    if (name.isEmpty() || name.startsWith("=>")) name = labelName(referred);
    if (name.isEmpty()) name = parent < 0 ? QString("Model") : QString("Part %1").arg(tree.nodes.size());

    const int node = static_cast<int>(tree.nodes.size());
    PartNode part;
    part.name = name;
    part.parent = parent;
    if (XCAFDoc_ShapeTool::IsAssembly(referred))
    {
      part.leaf = false;
      tree.nodes.push_back(part);
      tree.shapes.emplace_back();
      TDF_LabelSequence components;
      XCAFDoc_ShapeTool::GetComponents(referred, components);
      for (Standard_Integer i = 1; i <= components.Length(); ++i)
        walkLabel(components.Value(i), location * XCAFDoc_ShapeTool::GetLocation(components.Value(i)), node, tree, depth + 1);
    }
    else
    {
      const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(referred);
      tree.nodes.push_back(part);
      tree.shapes.push_back(shape.IsNull() ? shape : shape.Moved(location));
    }
  }

  // Appends the triangulations of the faces (made by the mesher) to `mesh`.
  void extractFaces(const TopoDS_Shape& shape, IndexedMesh& mesh)
  {
    // A first pass takes the triangulations and fixes where each one goes, so
    // that the second can fill the vectors in parallel.
    struct FaceTask
    {
        Handle(Poly_Triangulation) triangulation;
        TopLoc_Location location;
        bool reversed;
        size_t vertexBase, triangleBase;
    };
    std::vector<FaceTask> tasks;
    size_t totalNodes = 0, totalTriangles = 0;
    int unmeshed = 0;

    for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next())
    {
      const TopoDS_Face& face = TopoDS::Face(faces.Current());
      TopLoc_Location location;
      Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);
      if (triangulation.IsNull() || triangulation->NbTriangles() == 0)
      {
        ++unmeshed;
        continue;
      }
      tasks.push_back({triangulation, location, faces.Current().Orientation() != TopAbs_FORWARD, mesh.vertices.size() + totalNodes,
                       mesh.triangles.size() + totalTriangles});
      totalNodes += static_cast<size_t>(triangulation->NbNodes());
      totalTriangles += static_cast<size_t>(triangulation->NbTriangles());
    }
    if (unmeshed) spdlog::warn("{} faces of the CAD file could not be meshed", unmeshed);

    if (mesh.vertices.size() + totalNodes > 0xFFFFFFFFu) throw std::runtime_error("Too many vertices");
    mesh.vertices.resize(mesh.vertices.size() + totalNodes);
    mesh.triangles.resize(mesh.triangles.size() + totalTriangles);

    // The slots of the tasks are disjoint, so they need no locking.
    const int count = static_cast<int>(tasks.size());
#pragma omp parallel for schedule(dynamic)
    for (int t = 0; t < count; ++t)
    {
      const FaceTask& task = tasks[static_cast<size_t>(t)];
      const bool identity = task.location.IsIdentity();
      const gp_Trsf transformation = task.location.Transformation();

      const int nodes = task.triangulation->NbNodes();
      for (int n = 1; n <= nodes; ++n)
      {
        gp_Pnt p = task.triangulation->Node(n);
        if (!identity) p.Transform(transformation);
        mesh.vertices[task.vertexBase + static_cast<size_t>(n - 1)] = vsg::vec3(float(p.X()), float(p.Y()), float(p.Z()));
      }

      const int triangles = task.triangulation->NbTriangles();
      for (int i = 1; i <= triangles; ++i)
      {
        int a, b, c;
        task.triangulation->Triangle(i).Get(a, b, c);
        if (task.reversed) std::swap(b, c); // a reversed face winds the other way
        const uint32_t base = static_cast<uint32_t>(task.vertexBase);
        mesh.triangles[task.triangleBase + static_cast<size_t>(i - 1)] = vsg::uivec3(base + a - 1, base + b - 1, base + c - 1);
      }
    }
  }
} // namespace

CadDeflection cadDeflectionFromSettings()
{
  const QSettings settings;
  CadDeflection deflection;
  deflection.linear = settings.value("STEPLinearDeflection", deflection.linear).toDouble();
  deflection.angular = settings.value("STEPAngularDeflection", deflection.angular).toDouble();
  // A hand-edited or empty value must not make the mesher run for ever.
  if (!(deflection.linear > 0.0)) deflection.linear = CadDeflection().linear;
  if (!(deflection.angular > 0.0)) deflection.angular = CadDeflection().angular;
  return deflection;
}

void saveCadDeflection(const CadDeflection& deflection)
{
  QSettings settings;
  settings.setValue("STEPLinearDeflection", deflection.linear);
  settings.setValue("STEPAngularDeflection", deflection.angular);
}

bool isCadExtension(const QString& suffix)
{
  return suffix == "step" || suffix == "stp" || suffix == "iges" || suffix == "igs";
}

IndexedMesh readCad(const QString& path, const CadDeflection& deflection, const std::atomic<bool>* cancel, const LoadProgress& progress)
{
  initThreadPool();
  const QString suffix = path.section('.', -1).toLower();
  const bool step = suffix == "step" || suffix == "stp";
  if (!step && suffix != "iges" && suffix != "igs") throw std::runtime_error("Unknown CAD file extension: " + path.toStdString());
  const QByteArray utf8 = path.toUtf8();

  try
  {
    if (progress) progress(0);
    TopoDS_Shape shape;
    StepTree tree;
    if (step)
    {
      // Through XCAF, which keeps the assembly structure and the names that
      // STEPControl_Reader's OneShape() throws away.
      STEPCAFControl_Reader reader;
      reader.SetNameMode(true);
      reader.SetColorMode(false);
      reader.SetLayerMode(false);
      reader.SetPropsMode(false);
      reader.SetGDTMode(false);
      reader.SetSHUOMode(false);
      reader.SetMatMode(false);
      reader.SetViewMode(false);
      if (reader.ReadFile(utf8.constData()) != IFSelect_RetDone) throw std::runtime_error("Failed to read the STEP file");
      throwIfCancelled(cancel);
      if (progress) progress(5);
      Handle(Message_ProgressIndicator) transfer = new ProgressBridge(cancel, progress, 5, 20);
      Handle(TDocStd_Document) document = new TDocStd_Document("XmlXCAF");
      if (!reader.Transfer(document, transfer->Start())) throw std::runtime_error("Failed to transfer the STEP file");
      throwIfCancelled(cancel);

      Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
      TDF_LabelSequence roots;
      shapeTool->GetFreeShapes(roots);
      // A file with several roots gets a node of its own to hold them.
      const bool several = roots.Length() > 1;
      if (several)
      {
        tree.nodes.push_back({QFileInfo(path).completeBaseName(), -1, false, 0, 0});
        tree.shapes.emplace_back();
      }
      for (Standard_Integer i = 1; i <= roots.Length(); ++i) walkLabel(roots.Value(i), TopLoc_Location(), several ? 0 : -1, tree, 0);

      // The shape to mesh holds all the leaves.
      BRep_Builder builder;
      TopoDS_Compound all;
      builder.MakeCompound(all);
      for (const auto& leaf : tree.shapes)
        if (!leaf.IsNull()) builder.Add(all, leaf);
      shape = all;
      if (!hasShapes(shape)) // no usable structure: the file as one shape
      {
        tree = StepTree();
        shape = reader.Reader().OneShape();
      }
      if (!hasShapes(shape)) throw std::runtime_error("No transferable geometry found in the STEP file");
    }
    else
    {
      IGESControl_Reader reader;
      shape = readShape(reader, "IGES", utf8.constData(), cancel, progress);
    }

    // The tessellation, all the faces at once across the threads. The shape
    // keeps the triangulations, which is where extractFaces() finds them.
    IMeshTools_Parameters parameters;
    parameters.Deflection = deflection.linear;
    parameters.Angle = deflection.angular;
    parameters.Relative = Standard_False;
    parameters.InParallel = Standard_True;
    Handle(Message_ProgressIndicator) indicator = new ProgressBridge(cancel, progress, 20, 90);
    BRepMesh_IncrementalMesh mesher(shape, parameters, indicator->Start());
    throwIfCancelled(cancel);
    spdlog::info("Meshed {} with linear deflection {} and angular deflection {}", path.toStdString(), deflection.linear, deflection.angular);

    IndexedMesh mesh;
    if (tree.nodes.empty())
      extractFaces(shape, mesh);
    else
    {
      // Leaf by leaf, so that each owns a range of the triangles.
      for (size_t i = 0; i < tree.nodes.size(); ++i)
      {
        throwIfCancelled(cancel);
        PartNode& node = tree.nodes[i];
        node.firstTriangle = static_cast<uint32_t>(mesh.triangles.size());
        if (node.leaf && !tree.shapes[i].IsNull()) extractFaces(tree.shapes[i], mesh);
        node.endTriangle = static_cast<uint32_t>(mesh.triangles.size());
      }
      mesh.parts = std::move(tree.nodes);
      pruneParts(mesh.parts);
    }
    throwIfCancelled(cancel);
    if (mesh.triangles.empty()) throw std::runtime_error("The CAD file produced no triangles");
    if (progress) progress(100);
    return mesh;
  }
  catch (const Standard_Failure& failure) // not a std::exception
  {
    throw std::runtime_error(std::string("OpenCASCADE: ") + failure.GetMessageString());
  }
}
