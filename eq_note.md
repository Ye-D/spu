```shell
bazelisk build //libspu/mpc/aby3:mfippa_test --jobs 2
./bazel-out/k8-fastbuild/bin/libspu/mpc/aby3/mfippa_test
```

0. my view: 动态比特向量容器
1. 实现A2B双变体                              PPA 对比
2. 最高有效位提取我们的实现 的效率              MSB 对比ABY 2.0的电路，或者对比PPA 删掉一部分导线
3. 神经网络 算术电路不变，布尔电路修改          
4. 算术布尔乘法                               确认一下PPA最后的乘法能不能和bx融合
5. 多项式近似                                 布尔 and 布尔 and 算术