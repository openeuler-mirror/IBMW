# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

from pathlib import Path

from setuptools import find_packages, setup

package_name = 'ibmw_cli'
project_root = Path(__file__).resolve().parents[3]
version = (project_root / 'VERSION').read_text(encoding='utf-8').strip()

setup(
    name=package_name,
    version=version,
    packages=find_packages(exclude=['test']),
    install_requires=[
        'PyYAML>=5.4.1',
        'Jinja2>=3.0.3',
        'pyinstaller>=6.1.0',
        'autopep8>=1.6.0',
    ],
    author='IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame',
    author_email='fanglinxu@huawei.com',
    description='IB Middleware application python tools',
    license='Apache-2.0',
    entry_points={
        'console_scripts': [
            'ibmw_cli = ibmw_cli.main:main',
        ],
    },
    package_data={
        'ibmw_cli': [
            'templates/**/*',
            'templates/**/**/*',
        ],
    },
    include_package_data=True,
)
