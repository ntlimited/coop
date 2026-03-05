#include <cstring>

#include <gtest/gtest.h>

#include "coop/alloc.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "test_helpers.h"

namespace
{

// Simple type for testing construction/destruction.
//
struct Widget
{
    int value;
    bool* destroyed;

    Widget(int v, bool* d) : value(v), destroyed(d) {}
    ~Widget() { if (destroyed) *destroyed = true; }
};

// Interface + concrete pair for testing From<Concrete>().
//
struct IShape
{
    virtual ~IShape() = default;
    virtual int Area() = 0;
};

struct Rect : IShape
{
    int w, h;
    bool* destroyed;

    Rect(int w, int h, bool* d) : w(w), h(h), destroyed(d) {}
    ~Rect() override { if (destroyed) *destroyed = true; }
    int Area() override { return w * h; }
};

struct Circle : IShape
{
    int r;
    Circle(int r) : r(r) {}
    int Area() override { return r * r * 3; }
};

// Type with trailing flexible array pattern.
//
struct FlexBuf
{
    size_t len;
    char data[0];
};

} // end anon namespace

TEST(AllocTest, BasicConstruction)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        bool destroyed = false;
        {
            auto w = ctx->Allocate<Widget>(0, 42, &destroyed);
            EXPECT_EQ(w->value, 42);
            EXPECT_FALSE(destroyed);
        }
        EXPECT_TRUE(destroyed);
    });
}

TEST(AllocTest, TrailingBytes)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto fb = ctx->Allocate<FlexBuf>(128);
        fb->len = 128;
        memset(fb->data, 0xAB, 128);

        // Verify trailing bytes are accessible and distinct from the struct.
        //
        EXPECT_EQ(static_cast<unsigned char>(fb->data[0]), 0xAB);
        EXPECT_EQ(static_cast<unsigned char>(fb->data[127]), 0xAB);
    });
}

TEST(AllocTest, LIFOOrdering)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        bool d1 = false, d2 = false, d3 = false;
        {
            auto a = ctx->Allocate<Widget>(0, 1, &d1);
            {
                auto b = ctx->Allocate<Widget>(0, 2, &d2);
                {
                    auto c = ctx->Allocate<Widget>(0, 3, &d3);
                    EXPECT_EQ(c->value, 3);
                }
                EXPECT_TRUE(d3);
                EXPECT_FALSE(d2);
                EXPECT_FALSE(d1);
            }
            EXPECT_TRUE(d2);
            EXPECT_FALSE(d1);
        }
        EXPECT_TRUE(d1);
    });
}

TEST(AllocTest, InterfaceFrom)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        bool destroyed = false;
        {
            auto shape = coop::Alloc<IShape>::From<Rect>(ctx, 0, 3, 4, &destroyed);
            EXPECT_EQ(shape->Area(), 12);
            EXPECT_FALSE(destroyed);
        }
        // Virtual destructor dispatches to Rect::~Rect.
        //
        EXPECT_TRUE(destroyed);
    });
}

TEST(AllocTest, InterfaceFromDifferentConcretes)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        bool destroyed = false;
        {
            auto rect = coop::Alloc<IShape>::From<Rect>(ctx, 0, 5, 6, &destroyed);
            auto circle = coop::Alloc<IShape>::From<Circle>(ctx, 0, 10);
            EXPECT_EQ(rect->Area(), 30);
            EXPECT_EQ(circle->Area(), 300);
        }
        EXPECT_TRUE(destroyed);
    });
}

TEST(AllocTest, AllocBuffer)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto buf = ctx->AllocateBuffer(256);
        EXPECT_EQ(buf.size(), 256u);
        memset(buf.data(), 0xCC, 256);
        EXPECT_EQ(static_cast<unsigned char>(buf[0]), 0xCC);
        EXPECT_EQ(static_cast<unsigned char>(buf[255]), 0xCC);
    });
}

TEST(AllocTest, FreeFunctions)
{
    test::RunInCooperator([](coop::Context*)
    {
        bool destroyed = false;
        {
            auto w = coop::Allocate<Widget>(0, 99, &destroyed);
            EXPECT_EQ(w->value, 99);
        }
        EXPECT_TRUE(destroyed);

        auto buf = coop::AllocateBuffer(64);
        EXPECT_EQ(buf.size(), 64u);
    });
}

TEST(AllocTest, DefaultNull)
{
    coop::Alloc<Widget> empty;
    EXPECT_EQ(empty.get(), nullptr);
    // Destructor should be safe on null.
    //
}

TEST(AllocTest, AllocBufferDefaultNull)
{
    coop::AllocBuffer empty;
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.size(), 0u);
}

TEST(AllocTest, HeapDoesNotOverlapStack)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Allocate several objects and verify they don't overlap each other.
        // Each allocation should be at a higher address than the previous.
        //
        auto a = ctx->Allocate<Widget>(0, 1, nullptr);
        auto b = ctx->Allocate<Widget>(0, 2, nullptr);
        auto c = ctx->Allocate<Widget>(0, 3, nullptr);

        uintptr_t pa = reinterpret_cast<uintptr_t>(a.get());
        uintptr_t pb = reinterpret_cast<uintptr_t>(b.get());
        uintptr_t pc = reinterpret_cast<uintptr_t>(c.get());

        EXPECT_LT(pa, pb);
        EXPECT_LT(pb, pc);

        // All should be within the segment.
        //
        uintptr_t bottom = reinterpret_cast<uintptr_t>(ctx->m_segment.Bottom());
        uintptr_t top = reinterpret_cast<uintptr_t>(ctx->m_segment.Top());
        EXPECT_GE(pa, bottom);
        EXPECT_LT(pc + sizeof(Widget), top);
    });
}
