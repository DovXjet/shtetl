#pragma once

#include <vsgQt/Viewer.h>

#include <QCoreApplication>

// vsgQt::Viewer that renders a frame only when something has asked for one.
class COnDemandViewer : public vsg::Inherit<vsgQt::Viewer, COnDemandViewer>
{
  public:
    using vsg::Inherit<vsgQt::Viewer, COnDemandViewer>::Inherit;

    void render(double simulationTime = vsg::Viewer::UseTimeSinceStartPoint) override
    {
        if (!continuousUpdate && requests.load() == 0) return;

        if (advanceToNextFrame(simulationTime))
        {
            handleEvents();
            update();
            recordAndSubmit();
            present();
        }
        else if (status->cancel())
        {
            QCoreApplication::quit();
        }

        requests = 0;
    }
};
