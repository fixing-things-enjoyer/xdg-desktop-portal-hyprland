#include "mainpicker.h"
#include "./ui_mainpicker.h"
#include <QDebug>
#include <QtWidgets>

MainPicker::MainPicker(QWidget* parent) : QMainWindow(parent), ui(new Ui::MainPicker) {
    ui->setupUi(this);

    QCheckBox* rotationParam = new QCheckBox("Enable Rotation Fix", this);
    rotationParam->setObjectName("rotationCheck");
    ui->verticalLayout->addWidget(rotationParam);
}

MainPicker::~MainPicker() {
    delete ui;
}

void MainPicker::onMonitorButtonClicked(QObject* target, QEvent* event) {
    qDebug() << "click";
}
