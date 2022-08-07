// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2022.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Jihyung Kim $
// $Authors: Jihyung Kim $
// --------------------------------------------------------------------------

#include <OpenMS/VISUAL/APPLICATIONS/FLASHDeconvQWizardBase.h>
#include <ui_FLASHDeconvQWizardBase.h>

#include <OpenMS/APPLICATIONS/ToolHandler.h>
#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/CONCEPT/LogStream.h>
#include <OpenMS/VISUAL/APPLICATIONS/MISC/QApplicationTOPP.h>
#include <OpenMS/VISUAL/DIALOGS/FLASHDeconvQTabWidget.h>

//Qt
#include <QtCore/QDir>
#include <QDesktopServices>
#include <QMessageBox>
#include <QSettings>

using namespace std;
using namespace OpenMS;

namespace OpenMS
{
  using namespace Internal;

  FLASHDeconvQWizardBase::FLASHDeconvQWizardBase(QWidget* parent) :
    QMainWindow(parent),
    DefaultParamHandler("FLASHDeconvQWizardBase"),
    //clipboard_scene_(nullptr),
    ui(new Ui::FLASHDeconvQWizardBase)
  {
    ui->setupUi(this);
    QSettings settings("OpenMS", "FLASHDeconvQWizard");
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
    setWindowTitle("FLASHDeconvQWizard");
    setWindowIcon(QIcon(":/FLASHDeconvWizard.png"));

    FLASHDeconvQTabWidget* cwidget = new FLASHDeconvQTabWidget(this);
    setCentralWidget(cwidget);
  }


  FLASHDeconvQWizardBase::~FLASHDeconvQWizardBase()
  {
    delete ui;
  }


  void FLASHDeconvQWizardBase::showAboutDialog()
  {
    QApplicationTOPP::showAboutDialog(this, "FLASHDeconvQWizard");
  }

  void OpenMS::FLASHDeconvQWizardBase::on_actionExit_triggered()
  {
      QApplicationTOPP::exit();
  }

  void OpenMS::FLASHDeconvQWizardBase::on_actionVisit_FLASHDeconvQ_homepage_triggered()
  {
    const char* url = "https://www.openms.de/comp/flashdeconvq/";
    if (!QDesktopServices::openUrl(QUrl(url)))
    {
      QMessageBox::warning(0, "Cannot open browser. Please check your default browser settings.", QString(url));
    }
  }

  void OpenMS::FLASHDeconvQWizardBase::on_actionReport_new_issue_triggered()
  {
    const char* url = "https://github.com/OpenMS/OpenMS/issues"; // TODO: change?
    if (!QDesktopServices::openUrl(QUrl(url)))
    {
      QMessageBox::warning(0, "Cannot open browser. Please check your default browser settings.", QString(url));
    }
  }

} //namespace OpenMS




