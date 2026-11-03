# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

import os
import sys
import platform
from jinja2 import Environment, PackageLoader


chmod_shell_name = [
    'linux_start_shell',
    'format_shell',
    'build_sh',
    'test_sh',
]


class GeneratorBase:
    def __init__(self, output_dir=None):
        super(GeneratorBase, self).__init__()
        self.output_dir_ = output_dir

    def parse(self):
        pass

    def generate(self):
        raise NotImplementedError()

    @staticmethod
    def get_template_environment(template_info):
        if getattr(sys, 'frozen', False):
            template_package = template_info['template_package']
        else:
            template_package = 'ibmw_cli.' + template_info['template_package']
        template_file = template_info['template_file']

        file_loader = PackageLoader(template_package, ".")
        env = Environment(loader=file_loader)
        template = env.get_template(template_file)
        return template

    def output(self, template_info, name, output_stream):
        if isinstance(template_info['output_path'], str):
            output_path = template_info['output_path']
        else:
            output_path = template_info['output_path'].render(name=name)

        if platform.system().lower() == "linux":
            output_dir = os.path.expanduser(self.output_dir_)
        else:
            output_dir = self.output_dir_
        output_path = os.path.join(output_dir, output_path)

        if isinstance(template_info['output_file'], str):
            # output file name is not Template.
            output_file = template_info['output_file']
        else:
            output_file = template_info['output_file'].render(name=name)
        output_file_path = os.path.join(output_path, output_file)

        # Names from the yaml file are part of the path: none may lead out of
        # the output directory (e.g. "../x" or an absolute path).
        real_output_dir = os.path.realpath(output_dir)
        if os.path.commonpath([real_output_dir, os.path.realpath(output_file_path)]) != real_output_dir:
            raise Exception("Generated file " + output_file_path + " is outside of the output directory "
                            + output_dir + ", check the names in your yaml file!")

        if not os.path.exists(output_path):
            print("creating folder:", output_path)
            os.makedirs(output_path)

        if os.path.exists(output_file_path):
            raise Exception("The file is already exist in" + output_file_path)
        else:
            with open(output_file_path, 'w') as f:
                print("creating file:", output_file_path)
                f.write(output_stream)

        if platform.system().lower() == "linux" and template_info['template_name'] in chmod_shell_name:
            # Owner-writable only: group/other must not be able to alter a
            # script the developer later executes.
            os.chmod(output_file_path, 0o755)
