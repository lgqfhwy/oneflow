from __future__ import absolute_import

import oneflow.python.framework.op_arg_util as op_arg_util
import oneflow.core.job.placement_pb2 as placement_pb
import oneflow.python.eager.symbol as symbol_util
import oneflow.core.job.sbp_parallel_pb2 as sbp_parallel_pb
import random


class BoxingToMiddle(object):
    def __init__(
        self,
        boxing_method,
        get_middle_parallel_desc_symbol,
        get_middle_sbp_parallel,
        verbose=False,
    ):
        self.boxing_method_ = boxing_method
        self.get_middle_op_arg_parallel_attr_ = MiddleOpArgParallelAttr(
            get_middle_parallel_desc_symbol, get_middle_sbp_parallel,
        )
        self.verbose_ = verbose

    @property
    def boxing_method(self):
        return self.boxing_method_

    @property
    def get_middle_op_arg_parallel_attr(self):
        return self.get_middle_op_arg_parallel_attr_

    @property
    def verbose(self):
        return self.verbose_


def MiddleOpArgParallelAttr(get_parallel_desc_symbol, get_sbp_parallel):
    def GetOpArgParallelAttr(
        builder, produced_blob_object, consumer_op_arg_parallel_attr
    ):
        return op_arg_util.OpArgParallelAttribute(
            get_parallel_desc_symbol(
                builder, produced_blob_object, consumer_op_arg_parallel_attr
            ),
            get_sbp_parallel(
                builder, produced_blob_object, consumer_op_arg_parallel_attr
            ),
            produced_blob_object.op_arg_parallel_attr.opt_mirrored_parallel,
        )

    return GetOpArgParallelAttr


def ReplaceProducerDeviceTag(new_device_tag):
    def Getter(builder, produced_blob_object, consumer_op_arg_parallel_attr):
        x_parallel_attr = produced_blob_object.op_arg_parallel_attr
        return TryReplaceDeviceTag(
            builder, x_parallel_attr.parallel_desc_symbol, new_device_tag
        )

    return Getter


def ProducerRandomParallelIdPerMachine(device_tag=None):
    def Getter(builder, produced_blob_object, consumer_op_arg_parallel_attr):
        return RandomParallelIdPerMachine(
            produced_blob_object.parallel_desc_symbol,
            device_tag=device_tag,
            builder=builder,
        )

    return Getter


def ConsumerRandomParallelIdPerMachine(device_tag=None):
    def Getter(builder, produced_blob_object, consumer_op_arg_parallel_attr):
        return RandomParallelIdPerMachine(
            consumer_op_arg_parallel_attr.parallel_desc_symbol,
            device_tag=device_tag,
            builder=builder,
        )

    return Getter


def ProducerParallelDesc(builder, produced_blob_object, consumer_op_arg_parallel_attr):
    return produced_blob_object.parallel_desc_symbol


def ConsumerParallelDesc(builder, produced_blob_object, consumer_op_arg_parallel_attr):
    return consumer_op_arg_parallel_attr.parallel_desc_symbol


def ReplaceConsumerDeviceTag(new_device_tag):
    def Getter(builder, produced_blob_object, consumer_op_arg_parallel_attr):
        parallel_desc_sym = consumer_op_arg_parallel_attr.parallel_desc_symbol
        return TryReplaceDeviceTag(builder, parallel_desc_sym, new_device_tag)

    return Getter


def BroadcastParallel(builder, produced_blob_object, consumer_op_arg_parallel_attr):
    sbp_parallel = sbp_parallel_pb.SbpParallel()
    sbp_parallel.broadcast_parallel.SetInParent()
    return sbp_parallel


def ProducerSbpParallel(builder, produced_blob_object, consumer_op_arg_parallel_attr):
    return produced_blob_object.op_arg_parallel_attr.sbp_parallel


def ConsumerSbpParallel(builder, produced_blob_object, consumer_op_arg_parallel_attr):
    return consumer_op_arg_parallel_attr.sbp_parallel


def TryReplaceDeviceTag(builder, parallel_desc_symbol, device_tag):
    if parallel_desc_symbol.device_tag == device_tag:
        return parallel_desc_symbol
    else:
        return ReplaceDeviceTag(parallel_desc_symbol, device_tag, builder=builder)


def ReplaceDeviceTag(parallel_desc_symbol, device_tag, builder=None):
    assert parallel_desc_symbol.device_tag != device_tag
    parallel_conf = placement_pb.ParallelConf()
    for device_name in parallel_desc_symbol.parallel_conf.device_name:
        triple = device_name.split(":")
        parallel_conf.device_name.append(
            "%s:%s:%s" % (triple[0], device_tag, triple[2])
        )
    if builder is None:
        return symbol_util.ParallelDescSymbol(
            parallel_desc_symbol.symbol_id, parallel_conf, device_tag
        )
    else:
        return builder.GetParallelDescSymbol(parallel_conf)


def RandomParallelIdPerMachine(parallel_desc_symbol, device_tag=None, builder=None):
    if device_tag is None:
        for device_name in parallel_desc_symbol.parallel_conf.device_name:
            _, device_tag, _ = device_name.split(":")
            break
    assert device_tag is not None
    parallel_conf = placement_pb.ParallelConf()
    for machine_id, dev_ids in parallel_desc_symbol.machine_id2device_id_list.items():
        dev_id = dev_ids[random.randint(0, len(dev_ids) - 1)]
        parallel_conf.device_name.append("%s:%s:%s" % (machine_id, device_tag, dev_id))
    if builder is None:
        return symbol_util.ParallelDescSymbol(
            parallel_desc_symbol.symbol_id, parallel_conf, device_tag
        )
    else:
        return builder.GetParallelDescSymbol(parallel_conf)