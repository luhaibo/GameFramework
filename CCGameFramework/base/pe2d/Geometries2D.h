﻿#ifndef GEOMETRIES2D_H
#define GEOMETRIES2D_H
#include "Geometries.h"
#include "math/vector2.h"

class Geo2DShape;

// 相交测试
struct Geo2DResult
{
    Geo2DResult();
    Geo2DResult(const Geo2DShape* body, bool inside, float distance, float distance2, const vector2& position, const vector2& normal);

    Geo2DResult(const Geo2DResult& r);
    const Geo2DResult& operator = (const Geo2DResult& r);

    const Geo2DShape* body{ nullptr };
    bool inside{ false };
    float distance{ FLT_MAX };
    float distance2{ FLT_MAX };
    vector2 position;
    vector2 normal;
};

// 2D平面对象基类
class Geo2DObject
{
public:
    Geo2DObject();
    virtual ~Geo2DObject();

    virtual Geo2DResult sample(vector2 ori, vector2 dst) const = 0;
};

// 2D平面对象操作符
class Geo2DOper : public Geo2DObject
{
public:
    enum OpType
    {
        t_none,
        t_intersect,
        t_union,
        t_subtract,
        t_complement,
    };

    Geo2DOper(OpType op, std::shared_ptr<Geo2DObject> o1, std::shared_ptr<Geo2DObject> o2);
    ~Geo2DOper();

    /**
     * \brief 采样求交点
     * \param ori 直线起点
     * \param dir 直线方向
     * \return 相交情况
     */
    Geo2DResult sample(vector2 ori, vector2 dir) const override;

    OpType op{ t_none };

    std::shared_ptr<Geo2DObject> obj1, obj2;
};

// 2D形状
class Geo2DShape : public Geo2DObject
{
public:
    enum ShapeType
    {
        t_none,
        t_circle,
        t_ngon,
        t_plane,
        t_capsule,
    };

    Geo2DShape(ShapeType shape, color L);
    ~Geo2DShape();

    ShapeType shape{ t_none };
    color L;

    virtual vector2 get_center() const = 0;
};

// 圆
class Geo2DCircle : public Geo2DShape
{
public:
    Geo2DCircle(float cx, float cy, float r, color L);
    ~Geo2DCircle();

    Geo2DResult sample(vector2 ori, vector2 dir) const override;

    vector2 get_center() const override;

    vector2 center;
    float r, rsq;
};

class Geo2DFactory
{
public:
    using Geo2DObjPtr = std::shared_ptr<Geo2DObject>;

    static Geo2DObjPtr and(Geo2DObjPtr s1, Geo2DObjPtr s2);
    static Geo2DObjPtr or(Geo2DObjPtr s1, Geo2DObjPtr s2);
    static Geo2DObjPtr sub(Geo2DObjPtr s1, Geo2DObjPtr s2);

    static Geo2DObjPtr new_circle(float cx, float cy, float r, color L);
};

#endif // GEOMETRIES2D_H
