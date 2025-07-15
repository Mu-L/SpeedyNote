#ifndef CONTROLPANELDIALOG_H
#define CONTROLPANELDIALOG_H

#include <QDialog>
#include <QTabWidget>
#include <QComboBox>
#include <QColorDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QColor>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QLabel>
#include <QLabel>

#include "InkCanvas.h" // Needed for BackgroundStyle enum
#include "MainWindow.h"
#include "KeyCaptureDialog.h"
#include "ControllerMappingDialog.h" // New: Include controller mapping dialog

class ControlPanelDialog : public QDialog {
    Q_OBJECT

public:
    explicit ControlPanelDialog(MainWindow *mainWindow, InkCanvas *targetCanvas, QWidget *parent = nullptr);

private slots:
    void applyChanges();
    void chooseColor();
    void addKeyboardMapping();       // New: add keyboard shortcut
    void removeKeyboardMapping();    // New: remove keyboard shortcut
    void openControllerMapping();    // New: open controller mapping dialog
    void reconnectController();      // New: reconnect controller

private:
    InkCanvas *canvas;

    QTabWidget *tabWidget;
    QWidget *backgroundTab;

    QComboBox *styleCombo;
    QPushButton *colorButton;
    QSpinBox *densitySpin;

    QPushButton *applyButton;
    QPushButton *okButton;
    QPushButton *cancelButton;

    QColor selectedColor;

    void createBackgroundTab();
    void loadFromCanvas();

    MainWindow *mainWindowRef;
    InkCanvas *canvasRef;
    QWidget *performanceTab;
    QWidget *toolbarTab;
    void createToolbarTab();
    void createPerformanceTab();

    QWidget *controllerMappingTab;
    QPushButton *reconnectButton;
    QLabel *controllerStatusLabel;

    // Mapping comboboxes for hold and press
    QMap<QString, QComboBox*> holdMappingCombos;
    QMap<QString, QComboBox*> pressMappingCombos;

    void createButtonMappingTab();
    void createControllerMappingTab(); // New: create controller mapping tab
    void createKeyboardMappingTab();  // New: keyboard mapping tab

    // Keyboard mapping widgets
    QWidget *keyboardTab;
    QTableWidget *keyboardTable;
    QPushButton *addKeyboardMappingButton;
    QPushButton *removeKeyboardMappingButton;
    
    // Theme widgets
    QWidget *themeTab;

    QCheckBox *useCustomAccentCheckbox;
    QPushButton *accentColorButton;
    QColor selectedAccentColor;
    
    // Color palette widgets
    QCheckBox *useBrighterPaletteCheckbox;
    
    void createThemeTab();
    void chooseAccentColor();
    void updateControllerStatus(); // Update controller connection status display
};

#endif // CONTROLPANELDIALOG_H
